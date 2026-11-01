# hifi-receiver user manual

*Lite, Plus and Ultra. The one-page version is [`quick-start.md`](quick-start.md).
[`sources.md`](sources.md) lists every fact in this manual, where it comes
from, and which ones are still to be confirmed. Those are marked
**[TBD: …]** in the text.*

## Contents

1. [What it is](#1-what-it-is)
2. [Safety](#2-safety)
3. [The tiers at a glance](#3-the-tiers-at-a-glance)
4. [Connections](#4-connections)
5. [Power: which charger](#5-power-which-charger)
6. [Getting on the network](#6-getting-on-the-network)
7. [Playing music](#7-playing-music)
8. [Controls: app, buttons, remote, screen](#8-controls)
9. [Settings](#9-settings)
10. [Updates and factory reset](#10-updates-and-factory-reset)
11. [Status light](#11-status-light)
12. [Troubleshooting](#12-troubleshooting)
13. [Specifications](#13-specifications)
14. [For bring-up and testing](#14-for-bring-up-and-testing)
15. [Legal and licences](#15-legal-and-licences)

## 1. What it is

The hifi-receiver is a streaming amplifier: speakers in, music out.

- **Power:** it runs from a USB-C charger, so there is no mains wiring and no
  power brick.
- **Music:** it plays from Spotify, AirPlay, Bluetooth and a home music
  server, and from a DJ deck or any other line-level source on its RCA input.
- **Outputs:** it drives one pair of passive speakers directly, plus up to two
  powered subwoofers.
- **Control:** use the web app on your phone, the buttons, or a remote.

## 2. Safety

- **Connect speakers only with the power unplugged.**
- **Shorts:** a short across the speaker terminals, or from a terminal to the
  case, can stop the amplifier. It recovers when the short is removed.
- **One speaker per terminal pair.** Do not wire two speakers in parallel to
  one output. That makes 2 Ω, which this amplifier is not designed for.
- **Use the USB-C charger and cable this manual names.**
  - **Plus and Ultra:** run at 28 V on a 240 W (5 A) cable. A cheap 3 A
    cable can get hot at full power.
  - **Only a USB-C PD charger.** No adapters that turn other supplies into
    USB-C.
- **Ventilation:** leave **5 cm** clear above and behind the receiver. The
  Plus and Ultra get warm over the vents after long, loud playback. That
  is normal. When it gets hot, the receiver turns itself down a little
  until it cools.
  - **Don't cover the vents** with cloth, books or another device.
  - **Plus and Ultra breathe through the box:** air comes in through the
    slots low on the sides and front and underneath, and leaves through
    the grille in the lid. Stand it on its feet on a hard surface, not on
    a rug or cloth, and keep the lid grille open.
- **Loudness:** at full power the Plus and Ultra can reach over 105 dB at
  1 m. That level can damage hearing and speakers. Start low.
- **Use indoors and dry, 0–35 °C.**
- **No user-serviceable parts inside.** Open it only with the power
  unplugged. The enclosure is printed in PETG or ASA. A PLA replacement
  can soften over the amplifier, so don't use one.
- **Children:** the remote's coin battery, and any small parts, are a
  danger if swallowed. Keep them away from children.

## 3. The tiers at a glance

| | Lite | Plus | Ultra |
|---|---|---|---|
| Charger | USB-C PD, 20 V, 65 W or more | USB-C PD 3.1, 28 V, 140 W, with a 240 W cable | same as Plus |
| Power per channel, 4 Ω | 43 W | 77 W | 77 W (zone A + zone B share 140 W) |
| Speaker outputs | 1 pair | 1 pair | 2 pairs (zone A, zone B) |
| Subwoofer outputs (RCA) | 2 (left, right), full range | 2, with crossover, and level, delay and phase per sub | same as Plus |
| Line in (RCA) | yes, DJ direct only (zero delay) | DJ direct, or Line in through EQ | same as Plus, plus optical in |
| Network | Wi-Fi 2.4 GHz | Wi-Fi and Ethernet | Wi-Fi and Ethernet |
| Bluetooth | yes | yes | yes |
| Controls | 3 buttons on top | knob and 2 buttons | 2" colour screen, knob and 3 buttons |
| Remote in the box | no (it learns yours, see section 8) | yes | yes |

## 4. Connections

### Rear panel

**Lite, seen from behind, left to right:**

- **Speaker sockets**, four 4 mm banana sockets low down: **R− R+** (right
  speaker), then **L− L+** (left speaker). Red is +, black is −.
- **USB-C** power, low down.
- **SUB OUT**, an RCA pair.
- **LINE IN**, an RCA pair.

In each RCA pair the upper jack is **left** (white) and the lower one is
**right** (red). The printed case has no moulded labels yet, so the labels
are a sticker. The front has the remote's window, at the bottom right. The
top has the three buttons along the front edge and the status light to their
left. [TBD: Plus and Ultra rear panels: their enclosures aren't drawn yet]

| Label | What it is | Tiers |
|---|---|---|
| **USB-C** | power in, plus data for setup and testing | all |
| **LAN** | Ethernet, 100 Mb | Plus, Ultra |
| **LINE IN** L/R | RCA, line level, up to 2 V | all |
| **OPTICAL** | TOSLINK digital in, up to 24-bit/96 kHz | Ultra |
| **SUB OUT** L/R (Lite), **SUB 1**, **SUB 2** (Plus, Ultra) | RCA, to powered subwoofers | all |
| **L+ L− R+ R−** | speakers: 4 mm banana sockets | all |
| **ZONE B L+ L− R+ R−** | second speaker pair | Ultra |

### Speakers

- **Impedance:** 4 Ω, 6 Ω or 8 Ω speakers. 4 Ω is what the amplifier is
  designed around, and Vienna Acoustics speakers are 4 Ω.
- **Wire:** 1.5 mm² (16 AWG) is fine for runs up to 10 m.
- **Polarity:** keep **+** to **+** and **−** to **−** on both speakers. If
  one is the wrong way round, the bass is thin and the sound seems to come
  from nowhere.
- **Not ground:** both speaker terminals carry the signal, so neither is
  ground. Don't connect a speaker **−** to anything else, such as a
  subwoofer's ground.
- **Terminals:** 4 mm banana sockets, red and black, 11 mm apart: four
  on Lite and Plus, eight on Ultra (zone A and zone B). They take **single banana plugs only**, no wider than 11 mm across
  the body, so neighbouring plugs can't touch. Dual plugs, spades and bare
  wire don't fit; put a pair of banana plugs on each speaker cable (they
  come with most speaker cable, or cost a few dollars).

### Subwoofers

- **Connecting:** connect one or two powered (active) subwoofers with
  ordinary RCA cables.
- **Lite:** **SUB OUT** carries the full-range left signal on the white jack
  and the right on the red one, the same as the speakers. Put a sub on each,
  or one sub on either jack. Set each subwoofer's own crossover to about
  80 Hz. The app sets the level of both together.
- **Plus and Ultra:** **SUB 1** and **SUB 2** carry the bass only. The
  receiver does the crossover, so set the subwoofer's crossover to its
  highest setting, or **LFE/bypass** if it has one. Then tune it in the app
  (section 9). Each sub has its own trim, delay and phase.

### Line in: DJ deck, TV, phone

- **What it takes:** LINE IN is for line-level sources such as a DJ
  controller like the Pioneer DDJ-FLX4 (use its master RCA out), a CD
  player, a TV's audio out, or a phone through a 3.5 mm-to-RCA cable.
- **Turntables:** connect only through a phono preamp. A turntable
  straight in will be very quiet and thin.
- **Choosing it:** in the app, **Play → Input → DJ direct**. The line in
  goes straight to the amplifier with no delay, for a DJ deck cueing on
  headphones. Streaming, EQ and the sub settings do not apply. Choose
  **Streaming** to go back.
- **Plus and Ultra** also have **Line in**, which takes the line input
  through the digital path, so the volume, EQ, the subs and the power
  limiter apply. That adds about 0.1 s of delay: fine for a CD player or a
  TV, but a DJ cueing on headphones will hear it, so DJs use **DJ
  direct**. Lite has DJ direct only.
- **DJ direct on Plus and Ultra:** the subs play too, about 10 ms behind
  the speakers, at the sub level you set. If a long, loud set would
  overheat the amplifier, the receiver moves itself to **Line in**, which
  turns the music down a little instead of letting the amplifier shut
  off. Choose **DJ direct** again once it has cooled; it is also back after
  a restart.
- **Lite on a 65 W charger:** the power limiter (section 5) can't see DJ
  direct, because it is analog. A deck at full level, played very loud into
  4 Ω, can make a 65 W charger cut out. Turn the deck's master down a little
  if it does.

### Optical (Ultra)

**OPTICAL** takes a TV's or game console's TOSLINK output in stereo PCM.
Set the TV's audio output to **PCM** or **Stereo**, not Dolby or DTS.

## 5. Power: which charger

The receiver asks the charger for the voltage it wants. If the charger can't
give it, the receiver takes the next best and tells you in the app.

| Charger | Lite | Plus / Ultra |
|---|---|---|
| 140 W USB-C PD 3.1 (28 V), with a 240 W cable | full power: 43 W/ch | **full power: 77 W/ch** |
| 65–100 W USB-C PD (20 V) | **full power: 43 W/ch** | about half: 39 W/ch |
| 45 W USB-C PD | plays, lower volume ceiling | plays, lower volume ceiling |
| phone charger, laptop USB port (5 V) | starts, amplifier off | starts, amplifier off |

Lite needs at least 15 V from the charger to turn its amplifier on: a phone
charger that only gives 9 V or 12 V starts it with the amplifier off.

**Chargers that work:**

- **140 W:** Apple 140 W USB-C, Anker 140 W, Ugreen 140 W.
- **65 W or more:** any USB-C PD laptop charger, for Lite.
- **Cable:** the cable has to be rated **240 W / 5 A / EPR** for 28 V. With
  an ordinary cable, a 140 W charger gives 20 V only.

**Why it may cut out at high volume.** A USB-C charger switches off if you
draw more than it is rated for. The receiver stops that happening with a
**power limiter**:

- It keeps its average draw under 90% of what the charger offers.
- On a smaller charger, loud passages are held back slightly instead of
  cutting out.
- If you still hear dropouts, the charger is not delivering what it
  promises, so try another one.

**Standby:**

- **Automatic:** after 10 minutes of silence the amplifier switches off. It
  comes back as soon as something plays. The rest of the receiver stays on
  the network, so Spotify and AirPlay still find it.
- **By hand:** hold **▶** for 1 second (Lite), or use **Standby** on a
  remote you have taught (section 8).
- **Supply out of range:** if the charger's voltage drops too low or goes too
  high, the speakers switch off and the app says **Speakers off: the supply
  is out of range**. They come back by themselves a second after the supply
  is good again.

## 6. Getting on the network

The receiver needs a network for Spotify, AirPlay, the app and updates.
Bluetooth and line in work without one.

### Ethernet (Plus, Ultra)

Plug a network cable from your router into **LAN**. The receiver gets an
address automatically. Wi-Fi setup is not needed.

### Wi-Fi

The receiver uses 2.4 GHz Wi-Fi, which nearly every home router provides.

1. **Start setup.** If it has not been set up yet, or after a factory reset,
   the receiver makes its own Wi-Fi network, named **`hifi-lite-…`** (the
   name followed by a short code). Its password is **`squeezelite`**. The
   status light blinks.
2. **Join it.** On your phone or laptop, join that network. The app opens by
   itself on its **Setup** page with a welcome note. If it doesn't, browse to
   **http://192.168.4.1**.
3. **Connect.** Under **Wi-Fi**, press **Scan for networks**, then **Choose**
   your home network. Type its password and press **Join**.
4. **Done.** The page says **Joined …**. Put the phone back on your home
   Wi-Fi and open **http://hifi-lite.local** (*Finding it on the network*,
   below).

**If the page says it could not join,** the reason is shown, usually a
wrong password. The receiver's own network stays up, so type it again and
press **Join**.

**If it doesn't join:**

- **Name or password:** check them. The password is case-sensitive.
- **2.4 GHz:** if the router has separate 2.4 GHz and 5 GHz networks, pick
  the 2.4 GHz one. The receiver cannot use 5 GHz.
- **Hotel or guest Wi-Fi:** a network with a sign-in page will not work.
- **The setup network comes back** if the receiver cannot reach the saved
  network [TBD: bring-up: how long it waits first].

### Finding it on the network

- **The address** depends on the tier:

  | Tier | Address |
  |---|---|
  | Lite | **http://hifi-lite.local** |
  | Plus | **http://hifi-plus.local** |
  | Ultra | **http://hifi-ultra.local** |

  This manual writes `hifi-lite.local`. Use your tier's address instead.
- **Renaming:** if you have more than one receiver, rename each in
  **Setup → Name**. That is the name Spotify, AirPlay and Bluetooth show.
  The `.local` address stays the same. If two receivers of the same tier
  are on one network, the second answers as `hifi-lite-2.local`.
- **If the `.local` address doesn't open**, which happens on some Android phones and
  older Windows:
  - find the receiver's IP address in your router's list of devices, and
    open `http://<that address>`. Once you are in, **Setup → About and
    diagnostics** shows it as **Address**.

## 7. Playing music

| Source | How | Notes |
|---|---|---|
| **Spotify** | In the Spotify app, tap the devices icon and choose the receiver. | Needs Spotify Premium. The phone, the receiver and Spotify must be on the same network the first time. |
| **AirPlay** | iPhone/iPad: Control Centre → AirPlay. Mac: the sound menu. Any app: the AirPlay icon. | AirPlay 1. It shows as an AirPlay speaker. It does not join AirPlay 2 multi-room groups in the Home app. |
| **Bluetooth** | In your phone's Bluetooth settings, pick the receiver (`hifi-lite` unless you renamed it). | There is no pairing button: the receiver can be found whenever no other phone is connected to it. Remembers [TBD: bring-up: how many phones]. |
| **Tidal, Qobuz, Deezer, radio** | Play from the service's app over AirPlay. Or use a home music server (below). | |
| **Home music server** | Run Lyrion Music Server (free) on any always-on computer or NAS. The receiver finds it on its own. | Your own files, internet radio and many services. Also multi-room: several receivers in sync. |
| **Line in** | In the app, **Play → Input → DJ direct** (Plus, Ultra: or **Line in**). | Section 4. |
| **Optical** (Ultra) | In the app, **Play → Input → Optical**. | |

**Bluetooth:** to switch to another phone, disconnect the first one in its
Bluetooth settings. The receiver can then be found again.

**Switching source:**

- **Streams:** there is no source picker. A stream starts when a phone or
  computer plays to the receiver, as with any Spotify Connect or AirPlay
  speaker, and the app shows what is playing. [TBD: bring-up: which stream
  wins when a second one starts]
- **Line in and optical:** these are picked by hand under **Input**, because
  a line input is always "playing". Choose **Streaming** to go back.

**Hi-res:** Lite plays 16-bit audio at up to 96 kHz, which is already more
than its audio chip can resolve. [TBD: firmware: Plus and Ultra's bit depth
for the Sabre DAC] Spotify and Bluetooth are lower than that by nature. A
home music server gets the most from it.

## 8. Controls

### Web app

Open **http://hifi-lite.local** (Plus: `hifi-plus.local`, Ultra:
`hifi-ultra.local`) in any browser on the same network.

- **Keeping it like an app:**
  - iPhone: Safari → Share → **Add to Home Screen**.
  - Android: Chrome → ⋮ → **Add to Home screen**. It is a shortcut, not
    a full install, because Chrome only installs from `https` sites, and
    a device on your home network can't have one. It works the same way.
- **What's in it:** three tabs along the bottom. The app shows only the
  controls your tier has.
  - **Play:** what is playing and from where, **Previous**, **Play /
    pause** and **Next**, **Volume** and **Mute**, and **Input**:
    **Streaming** or **DJ direct** (Plus and Ultra add **Line in**, Ultra
    adds **Optical**). On Ultra, **Zone B speakers**.
  - **Sound:** the **Equaliser**, the **Subwoofers** and the **Volume
    limit**.
  - **Setup:** **Wi-Fi**, **Name**, **Remote**, **Updates**, and **About and
    diagnostics**: the model, firmware, address, charger, supply voltage,
    the amplifier's state (and temperature on Plus and Ultra), **Run
    self-test**, **Restart** and **Factory reset**.
- **Notices** at the top of **Play**:
  - **Speakers off: charger too weak.** The port gives too little voltage
    for the amplifier (section 5).
  - **Speakers off: the supply is out of range.** The charger's voltage is
    too low or too high. Try another charger or cable.
  - **Power limit.** The receiver has turned the sound down a little to
    stay within what the charger can give.
  - **Update failed.** The old firmware is still in place, so the
    receiver keeps working. Try again.
  - **Amplifier fault.** The amplifier switched itself off to protect the
    speakers. Check the speaker wires for a short, then press **Clear**.
- **"Looking for the receiver…"** means the app can't reach it. Check that
  it is powered and that the phone is on the same Wi-Fi.
- **Not in the first firmware yet:** the volume slider, **Mute**, the
  **Volume limit**, what is playing, and teaching a remote need a firmware
  update before they work. Until then, change the volume from the phone
  that is playing or with the buttons. [TBD: firmware: the update that adds
  them]
- **Advanced settings:** the streaming software's own settings page is at
  **http://hifi-lite.local/squeezelite/**. You don't need it for everyday
  use.

Lyrion control apps such as Material Skin, iPeng and Squeezer also control
it when you use a Lyrion server.

### Buttons

On Lite the three buttons are on top, along the front edge, left to right:

| Lite | Press | Hold 1 s |
|---|---|---|
| **−** | volume down | previous track |
| **+** | volume up | next track |
| **▶** | play/pause | standby |

There is no double press.

| Plus | Turn | Press | Hold |
|---|---|---|---|
| **Knob** | volume | play/pause | [TBD: firmware: standby?] |
| **SOURCE** | | next input [TBD: firmware] | [TBD: firmware] |
| **PRESET** | | play preset 1 [TBD: firmware: what a preset is] | store the current stream as the preset [TBD] |

**Ultra:** the knob and three buttons are on the front panel next to the
screen, left to right:

| Ultra | Turn | Press | Hold 1 s |
|---|---|---|---|
| **Knob** | volume | play/pause | standby |
| **Button 1** | | next input (Streaming, Line in, DJ direct) | mute |
| **Button 2** | | previous track | |
| **Button 3** | | next track | |

The light under the screen is green when ready, blinks blue while it has
no network (setup), and blinks red if the amplifier has shut itself off
after repeated faults.

**Factory reset:** there is no button combination. Use the app (section
10).

### Remote

- **Plus and Ultra** come with a 21-key remote. [TBD: firmware: its key map;
  the remote's battery type]
- **Any tier can learn another remote,** for example the TV's, so one remote
  does both. This needs the firmware update above.
  1. In the app, open **Setup → Remote**.
  2. Tap **Teach** next to a function: **Volume up**, **Volume down**,
     **Mute**, **Play / pause**, **Next**, **Previous**, **Input** or
     **Standby**.
  3. Point the remote at the front of the receiver and press the key you
     want for it. The app says when it has it.
  4. Repeat for each function. **Forget all** clears them.
- Aim at the front panel, where the receiver's IR window is (Lite: at the
  bottom right).

### Screen (Ultra)

The 2" screen shows what is playing: the track and artist from Spotify,
AirPlay, Bluetooth or Lyrion. [TBD: firmware: the source, the volume and
the charger's power on screen.] It goes dark a minute after you last touch
a control when nothing is playing, and lights when you touch one.

## 9. Settings

| Setting | Where | What it does | Tiers |
|---|---|---|---|
| Name | Setup | The name in Spotify, AirPlay, Bluetooth and the `.local` address | all |
| Equaliser | Sound | 10 bands, 31 Hz to 16 kHz, ±12 dB. Presets: **Flat**, **Bass +**, **Loudness**, **Vocal**, **Treble −** | all |
| Sub outputs | Sound → Subwoofers | **On** or **Off** | all |
| Level | Sound → Subwoofers | The level of both subs, in dB | all |
| Crossover | Sound → Subwoofers | Where the subs take over the bass, 40–160 Hz | Plus, Ultra |
| Take the deep bass off the main speakers | Sound → Subwoofers | A high-pass on the mains, so they play louder cleanly | Plus, Ultra |
| Trim | Sound → Subwoofers → Sub 1, Sub 2 | Each sub's own level, −12 to +6 dB | Plus, Ultra |
| Delay | Sound → Subwoofers → Sub 1, Sub 2 | 0–20 ms, to line each sub up with the speakers | Plus, Ultra |
| Phase | Sound → Subwoofers → Sub 1, Sub 2 | **0°** or **180°**. Pick whichever sounds fuller at your seat. | Plus, Ultra |
| Volume limit | Sound | The highest the volume can go, from the app, the buttons or a remote: 30 to 100 | all |
| Input | Play | **Streaming**. **DJ direct**: the line in straight to the amplifier, no delay, no EQ. **Line in**: EQ and subs apply, 5–15 ms late. **Optical**. | DJ direct: all. Line in: Plus, Ultra. Optical: Ultra |
| Zone B speakers | Play | On/off and **Level** for the second pair | Ultra |
| Wi-Fi | Setup | Scan, join or **Forget** a network | all |
| Remote | Setup | Teach the receiver your remote's keys | all |

On Lite the sub outputs carry the full-range signal, and the app says to set
the crossover on the subwoofer itself.

**Setting up two subwoofers (Plus, Ultra):**

1. **Crossover:** set it to 80 Hz.
2. **Level:** play music with steady bass and adjust **Level** until the
   bass sounds even with the speakers. If one sub is louder than the other,
   even them up with each sub's **Trim**.
3. **Phase:** sit in your listening seat and switch each sub's **Phase**
   between 0° and 180°. Keep whichever setting gives more bass.
4. **Delay:** if a sub is much nearer than the speakers, set its delay to
   the distance difference. 1 ms is about 34 cm.

## 10. Updates and factory reset

**Updates:**

- **Online:** **Setup → Updates → Check for updates**, then **Install** if
  one is offered. If the app says **No update server: use a file**, there is
  no online update for your receiver yet. [TBD: firmware: where the
  receiver's updates are published]
- **From a file:** **Setup → Updates → From a file…** and pick the `.bin`
  firmware file.
- **While it updates:** the app shows **Downloading…** then **Installing…
  do not unplug.** Keep it plugged in. It says **Updated. The receiver is
  restarting.** and restarts by itself.
- **If it fails:** the app says **Update failed** and the old firmware is
  still in place. The receiver also keeps a recovery firmware of its own,
  which can install an update again if one is interrupted.

**Factory reset** erases everything the receiver has saved: the Wi-Fi
network, the name, the sound settings, the Bluetooth phones it knows and the
remote keys it has learned.

- **From the app:** **Setup → About and diagnostics → Factory reset**.
  The same card has **Restart**.
- **There is no button combination.** If the receiver has lost your Wi-Fi,
  its own `hifi-lite-…` network comes back (section 6), and the app is
  reachable on it.
- **Afterwards:** the status light blinks and the `hifi-lite-…` setup
  network comes back. Set it up again as in section 6.

## 11. Status light

Lite has one red status light on top, to the left of the buttons.

| Light | Meaning |
|---|---|
| blinking | Wi-Fi setup: join the `hifi-lite-…` network (section 6) |
| [TBD: bring-up: what it shows once on the network] | on the network, ready |

The light doesn't show Bluetooth, the charger or faults. The app does: see
the notices in section 8 and **Setup → About and diagnostics**.

[TBD: Plus and Ultra: Plus has one status LED, Ultra an RGB LED beside its
screen. Their patterns come with their firmware.]

## 12. Troubleshooting

| Problem | Likely cause | What to do |
|---|---|---|
| Light off | No power | Try another USB-C charger and cable. A data-only cable doesn't carry power. |
| Light on, no sound at all | Amplifier off on a 5 V charger | Use a PD charger (section 5). The app says **Speakers off: charger too weak**, and **Setup → About and diagnostics** shows the **Charger**. |
| No sound from one source | **DJ direct** still on, or volume at 0 | In the app, set **Play → Input** to **Streaming**. Check the volume on the phone that is playing. |
| No sound from one speaker | A loose wire or plug | Check the wires at both ends and that each banana plug is pushed fully home. |
| Thin bass, vague image | One speaker wired backwards | Check **+** to **+** on both. |
| Cuts out at high volume | Charger too small, or a 3 A cable on 28 V | Use a 140 W charger and a 240 W cable (Plus, Ultra) or a 65 W+ charger (Lite). On Lite in **DJ direct**, turn the deck down a little (section 4). |
| No sound, the app says **Speakers off: the supply is out of range** | The charger's voltage is wrong | Try another charger and cable. |
| Sound stops after loud playing for a long time, then returns | Overheating protection | Clear the vents. Leave space above. If the app shows **Amplifier fault**, press **Clear**. |
| Hum from the subwoofer | A ground loop between the sub and another mains device | Plug the sub into the same power strip as the other gear. Try a different RCA cable. |
| Spotify doesn't list it | Different network, or a guest network | Put the phone on the same Wi-Fi. Restart the Spotify app. |
| AirPlay doesn't list it | Same as above | Same as above. |
| The `.local` address won't open | Android or Windows lacks mDNS | Use the IP address from the router (section 6). |
| The phone doesn't list it in Bluetooth | Another phone is connected to it | Disconnect that phone, then look again (section 7). |
| DJ deck is late against the headphones | **Input** is **Line in** | Choose **DJ direct** (Plus, Ultra). |
| No sound from the DJ deck | **Input** is **Streaming** | Choose **Play → Input → DJ direct**. |
| Line in distorts | The source is too loud | Turn down the deck's master or the phone's volume to about 75%. |
| Optical: no sound | The TV is sending Dolby/DTS | Set the TV to PCM/Stereo out. |
| The volume slider, Mute or Volume limit does nothing | The first firmware lacks them | Use the buttons or the playing phone, and update the firmware (section 10). |
| Nothing helps | | Factory reset (section 10), then set it up again. |

## 13. Specifications

| | Lite | Plus | Ultra |
|---|---|---|---|
| Amplifier | TI TPA3118D2, class D | TI TPA3220, class D | 2 × TI TPA3220, class D |
| Power, 4 Ω, 1% THD, per channel | 43 W | 77 W | 77 W (140 W total for 4 channels) |
| Power, 8 Ω | 23 W | 43 W | 43 W |
| DAC | Everest ES8388 | ESS ES9018K2M Sabre (mains), TI PCM5102A (subs) | same as Plus |
| Digital audio | 16-bit, up to 96 kHz | [TBD: firmware: 16 or 32-bit] | same as Plus |
| Line in | RCA, 2 Vrms max | same, plus Everest ES7243E ADC | same, plus TOSLINK optical |
| Sub out | 2 × RCA (left, right), full range | 2 × RCA, bass only, 2.1 Vrms | same |
| Wi-Fi | 802.11 b/g/n, 2.4 GHz | same | same |
| Ethernet | | 100BASE-TX | 100BASE-TX |
| Bluetooth | A2DP sink, SBC | same | same |
| Streaming | Spotify Connect, AirPlay 1, Bluetooth, Lyrion (LMS) | same | same |
| Power in | USB-C PD, 20 V | USB-C PD 3.1 EPR, 28 V (20 V fallback) | same as Plus |
| Standby power | [TBD: bring-up measurement] | [TBD] | [TBD] |
| Speaker terminals | 4 × 4 mm banana sockets | 4 × 4 mm banana sockets | 8 × 4 mm banana sockets |
| Size | 106 × 96 × 47 mm | [TBD: enclosure] | [TBD] |
| Weight | [TBD: bring-up] | [TBD] | [TBD] |
| Enclosure | 3D-printed PETG or ASA | same | same |

The power figures are calculated, not yet measured. The Lite amplifier
measured 10.9 W into 6 Ω on 12 V in an earlier board, and the 20 V figure
follows from that. The bring-up measurements will replace them.
[TBD: bench: measured power and THD]

## 14. For bring-up and testing

This section is for whoever brings a new unit up. Owners don't need it.

- **USB-C to a laptop:** the USB-C port is also a serial port.
  - It shows up as a **CH340** USB-serial device at 115200 baud.
  - It gives the boot log and the console, and it is how the firmware is
    flashed.
  - A laptop port powers the receiver with the amplifier held off.
- **On the network:** the receiver advertises itself as **`_hifi._tcp`**
  over mDNS and has an HTTP test API under **`/api/v1/`**, documented in
  `docs/test-api.md`.
  - **What it reports:** `GET /api/v1/info` (model, firmware, what the tier
    can do) and `GET /api/v1/state`: the PD contract, the supply voltage,
    the power state, amplifier faults, the power limiter and the network.
    Lite has no input-current or temperature sensor, so those read `null`.
  - **What it runs:** `POST /api/v1/tone` (sine, sweep, pink noise,
    silence) and `POST /api/v1/selftest`.
- **On the console:** the same calls over the USB serial port: `hifi
  state`, `hifi selftest`, `hifi tone …`.
- **The bench scripts** that drive all of this are in `bench/`
  (`docs/bench.md`). `python3 bench/bench.py find` finds a board on USB or
  the network, and `python3 bench/bench.py bringup` runs the whole bring-up.

## 15. Legal and licences

- **Firmware:** based on the open-source squeezelite-esp32. Its source and
  licences are in its source tree, at the commit named in
  `firmware/upstream.lock`, with this receiver's changes in
  `firmware/patches/` and `firmware/components/`.
- **Built for its owner's use, not certified for sale:**
  - **Spotify:** uses the open-source cspot. Spotify has not certified it.
  - **AirPlay:** is AirPlay 1. Apple has not certified it.
  - **Bluetooth:** the receiver is not Bluetooth SIG qualified.
  - **Radio and EMC:** the receiver is not FCC or CE tested as a product.
    Its Wi-Fi/Bluetooth module (ESP32-WROVER-E) carries its own modular
    approval.
- **Trademarks:** Spotify, AirPlay, Bluetooth, Wi-Fi and the speaker brands
  are trademarks of their owners. They are named here only to say what the
  receiver works with.
