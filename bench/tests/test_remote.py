"""The relay side of the bench (docs/remote.md): signing, MQTT, the relay
transport, OTA with rollback, the loopback bring-up and provisioning."""
import base64
import io
import json
import os
import queue
import sys
import tempfile
import time
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from hifibench import api, cli, ed25519, mock, mqtt, relay, remote  # noqa: E402
from hifibench import provision  # noqa: E402
from hifibench.broker import Broker  # noqa: E402
from hifibench.serialport import open_port  # noqa: E402

# RFC 8032 section 7.1, tests 1-3: (secret, public, message, signature)
RFC8032 = [
    ("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
     "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"),
    ("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
     "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"),
    ("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
     "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
     "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"),
]


class Ed25519(unittest.TestCase):
    def test_rfc8032_vectors(self):
        for sk, pk, msg, sig in RFC8032:
            sk, pk, msg, sig = (bytes.fromhex(x) for x in (sk, pk, msg, sig))
            self.assertEqual(ed25519.public_key(sk), pk)
            self.assertEqual(ed25519.sign(sk, msg), sig)
            self.assertTrue(ed25519.verify(pk, msg, sig))
            self.assertFalse(ed25519.verify(pk, msg + b"x", sig))
            self.assertFalse(ed25519.verify(pk, msg, sig[:63] + bytes([sig[63] ^ 1])))


class Mqtt(unittest.TestCase):
    def setUp(self):
        self.br = Broker(logins={"a": "pa", "b": "pb"})

    def tearDown(self):
        self.br.close()

    def client(self, cid, user="a", pw="pa", **k):
        return mqtt.Client(self.br.url, cid, user, pw, **k).connect()

    def test_bad_login_is_refused(self):
        with self.assertRaises(mqtt.MqttError):
            self.client("x", "a", "wrong")

    def test_pub_sub_wildcards_and_qos1(self):
        a, b = self.client("a"), self.client("b", "b", "pb")
        b.subscribe("hifi/+/reply", 1)
        b.subscribe("other/#", 0)
        a.publish("hifi/dev1/reply", b"one", 1)
        a.publish("hifi/dev1/cmd", b"not for b", 1)
        a.publish("other/x/y", b"two", 0)
        got = [b.messages.get(timeout=2) for _ in range(2)]
        self.assertEqual(sorted((m.topic, m.payload) for m in got),
                         [("hifi/dev1/reply", b"one"), ("other/x/y", b"two")])
        self.assertTrue(b.messages.empty())
        a.close(), b.close()

    def test_retained_and_last_will(self):
        will = ("hifi/d/status", b'{"online": false}', 1, True)
        board = self.client("board", will=will)
        board.publish("hifi/d/status", b'{"online": true}', 1, retain=True)
        late = self.client("late")
        late.subscribe("hifi/+/status", 1)
        m = late.messages.get(timeout=2)
        self.assertEqual((m.payload, m.retain), (b'{"online": true}', True))
        board.drop()  # no DISCONNECT: the will goes out
        m = late.messages.get(timeout=2)
        self.assertEqual(json.loads(m.payload), {"online": False})
        later = self.client("later")
        later.subscribe("hifi/d/status")
        self.assertEqual(json.loads(later.messages.get(timeout=2).payload), {"online": False})

    def test_clean_disconnect_drops_the_will(self):
        w = self.client("w", will=("t/will", b"gone", 0, False))
        s = self.client("s")
        s.subscribe("t/#")
        w.close()
        s.publish("t/after", b"x")
        self.assertEqual(s.messages.get(timeout=2).topic, "t/after")


class RelayRig(unittest.TestCase):
    def setUp(self):
        self.br, self.sim, self.rb, self.cfg = mock.relay_rig(logins={"hive": "h", "board": "b"})
        self.link = relay.Link(self.cfg)
        self.dev = api.Device(relay.Relay(self.link, self.rb.dev, timeout=5))

    def tearDown(self):
        self.link.close()
        self.rb.close()
        self.br.close()


class RelayCalls(RelayRig):
    def test_status_and_round_trip(self):
        self.assertTrue(self.link.boards(0.2)[self.rb.dev]["online"])
        self.assertEqual(self.dev.info()["mac"], self.sim.mac)
        with self.assertRaises(api.ApiError) as e:
            self.dev.t.call("tone", {"type": "sine", "level_dbfs": -3}, post=True)
        self.assertEqual(e.exception.status, 400)

    def raw(self, payload: bytes) -> dict:
        q = queue.Queue()
        c = mqtt.Client(self.cfg["url"], "raw", "hive", "h").connect()
        c.on_message = q.put
        c.subscribe(f"hifi/{self.rb.dev}/reply", 1)
        c.publish(f"hifi/{self.rb.dev}/cmd", payload, 1)
        try:
            return json.loads(q.get(timeout=3).payload)
        finally:
            c.close()

    def test_a_forged_or_replayed_command_does_nothing(self):
        seed = relay.read_seed(self.cfg["key"])
        line = relay.envelope(self.rb.dev, 10**13, "POST", "/api/v1/amp", {"enable": True})
        other, _ = ed25519.keygen()
        self.assertEqual(self.raw(relay.sign_command(other, line))["status"], 401)
        self.assertEqual(self.raw(line + b"\nAAAA")["status"], 401)
        self.assertFalse(self.sim.amp_on)
        wrong = relay.envelope("000000000000", 10**13 + 1, "GET", "/api/v1/info", None)
        self.assertEqual(self.raw(relay.sign_command(seed, wrong))["status"], 401)
        good = relay.sign_command(seed, relay.envelope(self.rb.dev, 10**13 + 2, "GET", "/api/v1/info", None))
        self.assertEqual(self.raw(good)["status"], 200)
        self.assertEqual(self.raw(good)["status"], 409)   # the same command again
        old = relay.envelope(self.rb.dev, 10**13 + 3, "GET", "/api/v1/info", None, ts=int(time.time()) - 3600)
        self.assertEqual(self.raw(relay.sign_command(seed, old))["status"], 409)

    def test_ota_good_image_becomes_valid(self):
        img = os.urandom(3 * relay.CHUNK + 123)
        r = relay.ota_push(self.dev.t, img, "0.2.0", timeout=5, say=lambda m: None)
        self.assertEqual(r["result"], "valid")
        self.assertEqual(self.dev.info()["fw"], "0.2.0")
        self.assertEqual(self.dev.t.call("ota")["slot"], "ota_1")
        self.assertTrue(self.link.boards(0.1)[self.rb.dev]["online"])

    def test_ota_bad_image_rolls_back(self):
        img = os.urandom(5000) + mock.BAD_IMAGE
        r = relay.ota_push(self.dev.t, img, "0.3.0", timeout=5, say=lambda m: None)
        self.assertEqual(r["result"], "rolled_back")
        self.assertEqual(r["event"]["from"], "0.3.0")
        st = self.dev.t.call("ota")
        self.assertEqual((st["fw"], st["slot"], st["last_rollback"]["from"]), ("mock-0.1", "ota_0", "0.3.0"))

    def test_ota_resumes_where_the_board_is(self):
        img = os.urandom(2 * relay.CHUNK)
        t = self.dev.t
        import hashlib
        t.call("ota/begin", {"size": len(img), "sha256": hashlib.sha256(img).hexdigest(), "version": "x"}, post=True)
        t.call("ota/chunk", {"off": 0, "data": base64.b64encode(img[:relay.CHUNK]).decode()}, post=True)
        with self.assertRaises(api.ApiError) as e:
            t.call("ota/chunk", {"off": 0, "data": base64.b64encode(img[:10]).decode()}, post=True)
        self.assertEqual((e.exception.status, e.exception.body["next"]), (409, relay.CHUNK))
        # ota_push starts over with begin; a lost chunk mid-way is resumed from "next"
        real, dropped = t.call, []

        def lossy(op, body=None, post=False):
            if op == "ota/chunk" and body["off"] == relay.CHUNK and not dropped:
                dropped.append(1)
                body = dict(body, off=body["off"] + relay.CHUNK)   # as if the previous one went missing
            return real(op, body, post)
        t.call = lossy
        r = relay.ota_push(t, img, "0.4.0", timeout=5, say=lambda m: None)
        self.assertEqual((r["result"], dropped), ("valid", [1]))

    def test_ota_sha_mismatch(self):
        t = self.dev.t
        t.call("ota/begin", {"size": 4, "sha256": "0" * 64, "version": "x"}, post=True)
        t.call("ota/chunk", {"off": 0, "data": base64.b64encode(b"abcd").decode()}, post=True)
        with self.assertRaises(api.ApiError) as e:
            t.call("ota/finish", {}, post=True)
        self.assertEqual(e.exception.status, 400)


class Loopback(RelayRig):
    def test_capture_level_and_thd(self):
        remote.tone(self.dev, 1000, -20)
        m = remote.measure(self.dev, 1000, -20, settle_s=0)
        for c in m["channels"]:
            self.assertAlmostEqual(c["vpk"], 17.98 / 2 * 0.1, delta=0.01)   # one leg: half of 0.1 x 17.98 V
            self.assertLess(c["thdn"], 0.001)

    def test_capture_when_idle_is_refused(self):
        with self.assertRaises(api.ApiError) as e:
            remote.capture(self.dev)
        self.assertEqual(e.exception.status, 409)

    def test_one_channel_dead(self):
        self.sim.faults.add("amp_r_dead")   # the right channel's output is silent
        m = remote.measure(self.dev, 1000, -30, settle_s=0)
        self.sim.faults.discard("amp_r_dead")
        self.assertIn("vpk", m["channels"][0])
        self.assertIn("error", m["channels"][1])

    def test_quiet_bringup(self):
        remote.time.sleep = lambda s: None
        try:
            s = remote.bringup(self.dev, say=lambda m: None)
        finally:
            remote.time.sleep = time.sleep
        self.assertLessEqual(max(m["level_dbfs"] for m in s["levels"]), -30)
        self.assertFalse(any(v.startswith("fail") for v in s["verdicts"].values()), s["verdicts"])
        self.assertIn("power into a load", " ".join(s["not_measured"]))
        self.assertIn("gain within", remote.markdown(s))

    def test_bringup_on_5v_skips_the_loopback(self):
        self.sim.set_charger("laptop")
        s = remote.bringup(self.dev, say=lambda m: None)
        self.assertTrue(s["verdicts"]["loopback"].startswith("not measured"))
        self.assertEqual(s["levels"], [])


class Console(unittest.TestCase):
    def test_escaping_round_trips(self):
        body = {"ssid": 'my "home" net', "password": "a\\b c d"}
        line = api.console_line("wifi", body)
        self.assertTrue(line.startswith('api wifi "'))
        argv = api.split_argv(line)
        self.assertEqual(argv[:2], ["api", "wifi"])
        self.assertEqual(json.loads(argv[2]), body)
        self.assertEqual(api.console_line("info"), "api info")
        self.assertEqual(api.console_line("selftest", post=True), 'api selftest "{}"')


class Provision(unittest.TestCase):
    def test_against_the_mock_console(self):
        br, sim, rb, cfg = mock.relay_rig(logins={"hive": "h", "board": "b"})
        try:
            rb.close()           # the board starts without a relay: provisioning sets it
            sim.ip, sim.remote_connected = None, lambda: False
            started = []

            def connect(c):
                started.append(mock.RelayBoard(sim, c["url"], rb.trusted, c["user"], c["password"], c["prefix"]))
            sim.remote_hook = connect
            with tempfile.TemporaryDirectory() as d:
                rj = Path(d, "relay.json")
                rj.write_text(json.dumps(cfg))
                out = []
                code = provision.main(["--port", mock.serve_pty(sim), "--relay", str(rj), "--timeout", "10"],
                                      ask=lambda p: "home", ask_secret=lambda p: "pw", say=out.append)
            self.assertEqual(code, 0, out)
            self.assertEqual(sim.ssid, "home")
            self.assertTrue(any("240ac4000001" in o for o in out))
            link = relay.Link(cfg)
            self.assertTrue(link.boards(0.2)["240ac4000001"]["online"])
            link.close()
            started[0].close()
        finally:
            br.close()

    def test_no_board(self):
        out = []
        self.assertEqual(provision.main(["--port", "/nonexistent"] and [], say=out.append)
                         if not os.path.exists("/dev/ttyUSB0") else 2, 2)


class RemoteCli(unittest.TestCase):
    def test_mock_bringup_writes_a_report(self):
        remote.time.sleep = lambda s: None
        try:
            with tempfile.TemporaryDirectory() as d, redirect_stdout(io.StringIO()):
                code = cli.main(["--mock", "remote", "bringup", "--loud", "--max-dbfs", "-12"]
                                if False else ["--mock", "--out", d, "remote", "bringup", "--loud", "--max-dbfs", "-12"])
                s = json.loads(Path(d, "session.json").read_text())
                self.assertTrue(Path(d, "report.md").is_file())
        finally:
            remote.time.sleep = time.sleep
        self.assertEqual(code, 0)
        self.assertEqual(max(m["level_dbfs"] for m in s["levels"]), -12)

    def test_keygen(self):
        with tempfile.TemporaryDirectory() as d, redirect_stdout(io.StringIO()) as o:
            cli.main(["remote", "keygen", str(Path(d, "k"))])
            seed = relay.read_seed(str(Path(d, "k")))
            self.assertEqual(oct(Path(d, "k").stat().st_mode & 0o777), "0o600")
        self.assertIn(ed25519.public_key(seed).hex(), o.getvalue())


if __name__ == "__main__":
    unittest.main()
