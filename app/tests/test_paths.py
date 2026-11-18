"""The app's user paths, driven in headless Chrome against the mock: what a
person taps, and what they then see (hifi-lite-lab hifi-lite-tests.md,
"User paths"). Each test is one path from start to end.

    python3 app/tests/test_paths.py
"""
from __future__ import annotations

import json
import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from cdp import CHROME, Page  # noqa: E402
from test_app import Mock  # noqa: E402

POLL = 6  # seconds: the app polls every 2 s, so a change shows within one or two polls


@unittest.skipUnless(CHROME, "no Chrome on this machine")
class Paths(unittest.TestCase):
    def open(self, frag="", **kw) -> Page:
        self.m = Mock(**kw)
        self.addCleanup(self.m.close)
        self.p = Page(self.m.url + frag)
        self.addCleanup(self.p.close)
        self.p.wait("!!document.querySelector('#view .card h2')", what="the first page")
        return self.p

    def rx(self, fn):
        """Change the mock receiver's state as the firmware would on its own."""
        with self.m.srv.rx.lock:
            fn(self.m.srv.rx.s)

    def see(self, text, timeout=POLL):
        self.assertTrue(self.p.has(text, timeout), f"{text!r} not on the page; it reads:\n{self.p.text()[:800]}")

    def no(self, text):
        self.assertNotIn(text, self.p.text() + self.p.js("document.body.textContent"))

    def toast_is(self, text, timeout=POLL):
        self.p.wait(f"(document.querySelector('#toast').textContent || '').includes({json.dumps(text)}) && "
                    "!document.querySelector('#toast').hidden", timeout, what=f"toast {text!r}")

    # ---- first run and Wi-Fi

    def first_run_join(self, password):
        p = self.open(ap=True)
        self.see("Welcome.")
        self.assertEqual(p.js("document.querySelector('#tabs .on').dataset.tab"), "setup")
        p.click("Scan for networks")
        p.click("Choose", within="Wi-Fi")  # the first: Home
        self.assertEqual(p.js("__t.card('Wi-Fi').querySelector('input[type=text]').value"), "Home")
        p.type("input[type=password]", password, within="Wi-Fi")
        p.click("Join", within="Wi-Fi")
        self.toast_is("Joining Home")
        return p

    def test_first_run_join_with_the_right_password(self):
        p = self.first_run_join("hunter22")
        self.assertEqual(p.posts("wifi"), [{"ssid": "Home", "password": "hunter22"}])
        self.see("Joined Home. Reconnect this phone to it and open http://hifi-lite.local (192.168.1.40).")
        self.no("Welcome.")

    def test_first_run_join_with_a_wrong_password_says_so_in_time(self):
        p = self.first_run_join("wrong")
        self.see("Joining Home…")
        p.js("(() => { const n = Date.now; Date.now = () => n() + 60000; })()")  # past the join wait
        self.see("Could not join Home: check the password")
        self.see("Welcome.")  # still on its own network: try again from here

    def test_a_join_the_receiver_refuses_keeps_its_error(self):
        p = self.open(ap=True)
        self.m.srv.rx.fail["wifi"] = (500, "couldn't start joining")
        p.type("input[type=text]", "Home", within="Wi-Fi")
        p.click("Join", within="Wi-Fi")
        self.toast_is("couldn't start joining")
        time.sleep(0.5)
        self.toast_is("couldn't start joining", 0.1)  # not replaced by "Joining Home…"
        self.no("Joining Home")

    def test_join_with_no_network_chosen(self):
        p = self.open(ap=True)
        p.click("Join", within="Wi-Fi")
        self.toast_is("Choose a network")
        self.assertEqual(p.posts("wifi"), [])

    def test_forget_wifi_goes_back_to_setup(self):
        p = self.open("#setup")
        self.see("signal")
        p.click("Forget", within="Wi-Fi")
        self.see("Welcome.")
        self.see("http://hifi-lite.local")

    def test_losing_the_receiver_and_finding_it_again(self):
        p = self.open()
        self.see("Teardrop")
        self.m.srv.rx.fail["state"] = (503, "gone")
        self.see("Looking for the receiver")
        self.see("Teardrop")  # the last state stays under the notice
        self.assertFalse(p.js("document.querySelector('#conn').classList.contains('on')"))
        with self.m.srv.rx.lock:  # it comes back renamed and restarted
            self.m.srv.rx.info["name"] = "Lounge"
            self.m.srv.rx.t0 = time.time()
        del self.m.srv.rx.fail["state"]
        self.p.wait("document.querySelector('#name').textContent === 'Lounge'", POLL, what="the new name")
        self.no("Looking for the receiver")
        self.assertTrue(p.js("document.querySelector('#conn').classList.contains('on')"))

    def test_a_poll_the_receiver_never_answers_gives_up(self):
        # An update restarts the receiver mid-poll: that request never gets an
        # answer. The page must notice within a poll's limit, not the browser's
        # own timeout (minutes), and see the receiver as soon as it is back.
        self.open()
        self.see("Teardrop")
        self.m.srv.rx.fail["state"] = (None, "restarting")
        time.sleep(8.5)  # past the first poll's limit: one poll out of time is a busy receiver, not a lost one
        self.no("Looking for the receiver")
        self.see("Looking for the receiver", timeout=14)
        del self.m.srv.rx.fail["state"]
        t = time.time()
        self.p.wait("document.querySelector('#conn').classList.contains('on')", 10, what="the receiver back")
        self.assertLess(time.time() - t, 5)

    def test_rejoining_the_network_it_is_on_waits_for_the_firmware(self):
        p = self.open("#setup")
        self.see("signal")  # on Home already
        p.type("input[type=text]", "Home", within="Wi-Fi")
        p.type("input[type=password]", "wrong", within="Wi-Fi")
        p.click("Join", within="Wi-Fi")
        self.toast_is("Joining Home")
        time.sleep(2.5)  # a poll: still on Home, but the new password hasn't been tried yet
        self.no("Joined Home")
        p.js("(() => { const n = Date.now; Date.now = () => n() + 11000; })()")  # still on it after a while
        self.see("Joined Home.")

    def test_a_bug_in_a_page_is_not_a_lost_receiver(self):
        p = self.open()
        self.see("Teardrop")
        infos = len([u for m, u in [c[:2] for c in p.calls("GET")] if u.endswith("api/v1/info")])
        p.js("window.__play = PAGES.play; PAGES.play = () => { throw new Error('a bug'); }")
        time.sleep(4.5)  # two polls
        self.assertTrue(p.js("document.querySelector('#conn').classList.contains('on')"))
        self.assertEqual(len([u for m, u in [c[:2] for c in p.calls("GET")] if u.endswith("api/v1/info")]), infos)
        p.js("PAGES.play = window.__play")
        self.see("Teardrop")
        self.no("Looking for the receiver")

    def test_while_joining_the_lost_receiver_card_says_where_to_look(self):
        p = self.first_run_join("hunter22")
        self.m.srv.rx.fail["state"] = (503, "gone")  # the phone left the receiver's network
        self.see("reconnect this phone to Home and open http://hifi-lite.local")
        p.js("(() => { const n = Date.now; Date.now = () => n() + 301000; })()")  # long after the join
        self.see("Check that it is powered")
        self.no("If it joined")

    # ---- the name

    def test_rename(self):
        p = self.open("#setup")
        p.type("input[type=text]", "Lounge", within="Name")
        p.click("Save", within="Name")
        self.toast_is("Renamed")
        self.assertEqual(p.js("document.querySelector('#name').textContent"), "Lounge")
        p.tab("Play")
        self.see("Playing from Spotify")
        p.tab("Setup")
        self.assertEqual(p.js("__t.card('Name').querySelector('input').value"), "Lounge")

    def test_a_refused_name_says_why_and_keeps_the_old_one(self):
        p = self.open("#setup")
        p.type("input[type=text]", 'Tom "the" room', within="Name")
        p.click("Save", within="Name")
        self.toast_is('may not have "')
        time.sleep(0.5)
        self.toast_is('may not have "', 0.1)  # not "Renamed"
        self.assertEqual(p.js("document.querySelector('#name').textContent"), "HiFi Lite")
        p.type("input[type=text]", "   ", within="Name")
        p.click("Save", within="Name")
        self.toast_is("name is required")

    # ---- sources: what each looks like, with the name and volume

    def source(self, source, playing, want_title, want_head):
        self.rx(lambda s: s["audio"].update(source=source, playing=playing))
        self.see(want_head)
        # the heading can already be right from the step before: wait for the title
        self.p.wait(f"(document.querySelector('.np-title') || {{}}).textContent === {json.dumps(want_title)}", POLL,
                    what=f"the title {want_title!r}")

    def test_each_source_shows_what_plays(self):
        p = self.open()
        self.see("Playing from Spotify")
        for src, head in (("airplay", "AirPlay"), ("lms", "Lyrion"), ("bt", "Bluetooth")):
            self.source(src, {"state": "play", "title": f"Song on {head}", "artist": "A", "album": "B"},
                        f"Song on {head}", f"Playing from {head}")
            self.assertTrue(p.js("!!__t.find('Pause', 'button.icon')"), src)
            self.assertEqual(p.js("document.querySelector('#name').textContent"), "HiFi Lite")
            self.assertEqual(p.js("__t.card('Volume').querySelector('.big').textContent"), "35")
        # Bluetooth from a phone that sends no track details still plays
        self.source("bt", {"state": "play", "title": None}, "Playing", "Playing from Bluetooth")
        self.assertTrue(p.js("!!__t.find('Pause', 'button.icon')"))
        self.source("bt", {"state": "pause", "title": None}, "Paused", "Playing from Bluetooth")
        self.assertTrue(p.js("!!__t.find('Play', 'button.icon')"))
        self.source("idle", None, "Nothing playing", "Ready")
        self.see("Play to “HiFi Lite” from Spotify, AirPlay, Bluetooth or Lyrion.")
        self.assertFalse(p.js("!!__t.find('Pause', 'button.icon') || !!__t.find('Play', 'button.icon')"))

    def test_transport_buttons(self):
        p = self.open()
        p.click("Pause")
        p.wait("!!__t.find('Play', 'button.icon')", POLL, what="the Play button")
        p.click("Next")
        p.click("Previous")
        self.assertEqual([b["action"] for b in p.posts("control")], ["pause", "next", "prev"])

    def test_dj_direct_and_back(self):
        p = self.open()
        p.click("DJ direct", within="Input")
        self.see("Playing from an input")
        self.see("The RCA line in, straight to the amplifier.")
        self.assertEqual(p.js("document.querySelector('.np-title').textContent"), "DJ direct")
        p.tab("Sound")
        self.see("DJ direct is on, so the line in skips the EQ")
        p.tab("Play")
        p.click("Streaming", within="Input")
        self.see("Teardrop")
        self.assertEqual([b["input"] for b in p.posts("input")], ["dj_direct", "stream"])

    # ---- volume

    def test_volume_mute_and_the_limit(self):
        p = self.open()
        p.slide("Volume", 60)
        p.wait("__t.card('Volume').querySelector('.big').textContent === '60'", POLL, what="volume 60")
        p.click("Mute")
        p.wait("!!__t.find('Unmute')", POLL, what="Unmute")
        p.click("Unmute")
        p.tab("Sound")
        p.slide("Volume limit", 50)
        p.wait("document.querySelector('#maxv').textContent === '50'", POLL)
        p.tab("Play")
        p.wait("__t.card('Volume').querySelector('.big').textContent === '50'", POLL, what="volume held to 50")
        self.assertEqual(p.js("__t.find('Volume', 'input[type=range]').max"), "50")
        self.assertEqual(self.m.get("api/v1/state")["audio"]["volume"], 50)

    # ---- sound settings

    def test_sound_settings(self):
        p = self.open("#sound")
        p.click("Bass +", within="Equaliser")
        p.wait("__t.find('Bass +').className === 'on'", POLL, what="Bass + picked")
        p.click("Mono", within="Speaker output")
        p.wait("__t.find('Mono').className === 'on'", POLL)
        p.click("Off", within="Subwoofers")
        p.wait("!__t.card('Subwoofers').querySelector('input[type=range]')", POLL, what="the sub level gone")
        boost = "__t.card('Loudness').querySelectorAll('input[type=range]')"
        p.js(f"(() => {{ const e = {boost}[0]; e.value = 4.5; e.dispatchEvent(new Event('change', {{bubbles: true}})); }})()")
        self.see("Limiting now")
        p.js(f"(() => {{ const e = {boost}[1]; e.value = 12; e.dispatchEvent(new Event('change', {{bubbles: true}})); }})()")
        p.wait("__t.card('Loudness').textContent.includes('12 W')", POLL)
        a = self.m.get("api/v1/state")
        self.assertEqual(a["audio"]["eq"][:3], [6, 5, 3])
        self.assertEqual((a["audio"]["channel"], a["audio"]["sub"]["on"]), ("mono", False))
        self.assertEqual((a["audio"]["boost"]["db"], a["governor"]["heat_cap_w"]), (4.5, 12.0))
        p.click("Flat", within="Equaliser")
        p.wait("__t.find('Flat').className === 'on'", POLL)

    # ---- the remote

    def test_teach_a_remote_button_and_forget_all(self):
        p = self.open("#setup")
        p.js("[...__t.card('Remote').querySelectorAll('li')].find((l) => l.textContent.startsWith('Mute'))"
             ".querySelector('button').click()")
        self.see("Press the button on your remote for “Mute” now")
        self.see("✓", 8)
        p.click("Forget all", within="Remote")
        p.wait("!__t.card('Remote').textContent.includes('✓')", POLL, what="the ✓ gone")

    # ---- diagnostics, restart, reset

    def test_selftest_restart_and_reset(self):
        p = self.open("#setup")
        p.click("Run self-test")
        p.wait("__t.card('About and diagnostics').querySelectorAll('ul.list').length === 2", POLL, what="the self-test list")
        p.click("Restart")
        self.toast_is("Restarting")
        self.m.srv.rx.fail["reboot"] = (500, "busy")
        p.click("Restart")
        self.toast_is("busy")
        time.sleep(0.3)
        self.toast_is("busy", 0.1)
        p.click("Factory reset")
        self.toast_is("unplug the receiver and plug it back in within 10 minutes")
        self.assertTrue(self.m.srv.rx.armed)

    # ---- updates

    def test_update_check_install_and_restart(self):
        p = self.open("#setup")
        self.see("Firmware 0.1.0")
        p.click("Check for updates")
        self.see("Version 0.2.0 is available")
        p.click("Install")
        p.wait("/Downloading…|Installing… do not unplug./.test(document.body.innerText)", POLL, what="the progress")
        self.see("Firmware 0.2.0", 12)  # info read again after the restart
        self.see("Up to date.")
        self.no("Install")

    def test_a_failed_update_check_can_be_tried_again(self):
        p = self.open("#setup")
        self.m.srv.rx.fail["update/check"] = (502, "no answer from the update server")
        p.click("Check for updates")
        self.see("Couldn't check for updates: no answer from the update server")
        del self.m.srv.rx.fail["update/check"]
        p.click("Check again")
        self.see("Version 0.2.0 is available")

    # ---- power

    def test_weak_charger_then_a_good_one(self):
        p = self.open(pdo=(5.0, 3.0))
        self.see("Speakers off: charger too weak.")
        self.see("This port gives 5 V.")
        p.tab("Setup")
        self.see("5 V × 3 A (15 W) · PD")
        self.assertEqual(p.js("[...__t.card('About and diagnostics').querySelectorAll('li')]"
                              ".find((l) => l.textContent.startsWith('Amplifier')).lastChild.textContent"), "off")

    def test_amp_fault_clear(self):
        p = self.open(fault=True)
        self.see("Amplifier fault.")
        p.click("Clear")
        p.wait("!document.body.innerText.includes('Amplifier fault.')", POLL, what="the fault cleared")
        self.assertEqual(p.posts("amp"), [{"reset": True}])

    def test_power_graphs_read_back_and_go_quiet_when_the_receiver_is_lost(self):
        p = self.open("#setup")
        self.see("Last 15 minutes")
        time.sleep(4.5)  # a few polls of history
        # hover the right edge: the readout says the values now
        p.js("(() => { const c = document.querySelector('.cs'), r = c.getBoundingClientRect();"
             " c.dispatchEvent(new PointerEvent('pointermove', {bubbles: true, pointerType: 'mouse',"
             " clientX: r.right - 1, clientY: r.top + 10})); })()")
        self.assertRegex(p.js("document.querySelector('.rd').textContent"), r"out [0-9.]+ W · in [0-9.]+ W")
        self.m.srv.rx.fail["state"] = (503, "gone")
        self.see("Looking for the receiver")
        self.see("last seen")
        self.assertIn("–", p.js("document.querySelector('.c-h .now').textContent"))
        self.assertFalse(p.js("!!document.querySelector('.c-h .now.bad')"))
        p.tab("Play")  # the Power card dims and says when it last heard
        self.assertTrue(p.js("!!document.querySelector('.pw-n[data-off]')"))
        self.see("last seen")

    # ---- party

    def test_party_on_lead_and_off(self):
        p = self.open()
        p.click("On", within="Party")
        self.see("Leading: 1 more receiver plays along.")
        with self.m.srv.rx.lock:  # another one took the lead
            self.m.srv.rx.party.update(role="follower", leader="Kitchen", followers=[])
        self.see("Playing along with Kitchen.")
        p.click("Lead from this one")
        self.see("Leading: 1 more receiver plays along.")
        p.click("Off", within="Party")
        p.wait("!__t.card('Party').textContent.includes('Leading')", POLL)
        self.assertEqual([b["op"] for b in p.posts("party")], ["on", "lead", "off"])


if __name__ == "__main__":
    unittest.main()
