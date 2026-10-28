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
  Plus and Ultra heatsink gets hot after long, loud playback, so don't
  touch it then.
  - **Don't cover the vents** with cloth, books or another device.
- **Loudness:** at full power the Plus and Ultra can reach over 105 dB at
  1 m. That level can damage hearing and speakers. Start low.
- **Use indoors and dry, 0–35 °C.**
- **No user-serviceable parts inside.** Open it only with the power
  unplugged. The enclosure is printed in PETG or ASA. A PLA replacement
  can soften near the heatsink, so don't use one.
- **Children:** the remote's coin battery, and any small parts, are a
  danger if swallowed. Keep them away from children.

## 3. The tiers at a glance

| | Lite | Plus | Ultra |
|---|---|---|---|
| Charger | USB-C PD, 20 V, 65 W or more | USB-C PD 3.1, 28 V, 140 W, with a 240 W cable | same as Plus |
| Power per channel, 4 Ω | 43 W | 77 W | 77 W (zone A + zone B share 140 W) |
| Speaker outputs | 1 pair | 1 pair | 2 pairs (zone A, zone B) |
| Subwoofer outputs (RCA) | 2, full range | 2, with crossover, level, delay and phase | same as Plus |
| Line in (RCA) | yes, zero-latency | yes, plus DJ direct | yes, plus DJ direct and optical in |
| Network | Wi-Fi 2.4 GHz | Wi-Fi and Ethernet | Wi-Fi and Ethernet |
| Bluetooth | yes | yes | yes |
| Controls | 3 buttons | knob and 2 buttons | 2" colour screen, knob and 3 buttons |
| Remote in the box | no (it learns yours) | yes | yes |

## 4. Connections

### Rear panel

[TBD: enclosure and boards: rear panel drawing, left to right, with each
connector's printed label]

| Label | What it is | Tiers |
|---|---|---|
| **USB-C** | power in, plus data for setup and testing | all |
| **LAN** | Ethernet, 100 Mb | Plus, Ultra |
| **LINE IN** L/R | RCA, line level, up to 2 V | all |
| **OPTICAL** | TOSLINK digital in, up to 24-bit/96 kHz | Ultra |
| **SUB 1**, **SUB 2** | RCA, to powered subwoofers | all |
| **SPEAKERS L+ L− R+ R−** | 4 mm banana or bare wire | all |
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
- [TBD: Lite board: whether the speaker terminals take banana plugs or are
  binding posts]

### Subwoofers

- **Connecting:** connect one or two powered (active) subwoofers to **SUB 1**
  and **SUB 2** with ordinary RCA cables.
- **Lite:** the sub outputs carry the full-range signal. Set the
  subwoofer's own crossover to about 80 Hz.
- **Plus and Ultra:** the receiver does the crossover, so set the
  subwoofer's crossover to its highest setting, or **LFE/bypass** if it has
  one. Then tune it in the app (section 9).
- **Two subs:** both get the same bass, in mono, with one level, delay and
  phase setting for the pair [TBD: web app/firmware: product.md plans a
  setting per sub]. Balance the two with the subs' own level knobs.

### Line in: DJ deck, TV, phone

- **What it takes:** LINE IN is for line-level sources such as a DJ
  controller like the Pioneer DDJ-FLX4 (use its master RCA out), a CD
  player, a TV's audio out, or a phone through a 3.5 mm-to-RCA cable.
- **Turntables:** connect only through a phono preamp. A turntable
  straight in will be very quiet and thin.
- **Zero latency:**
  - **Lite** routes the line input straight to the amplifier, so it has no
    delay.
  - **Plus and Ultra** have a **DJ direct** mode that does the same (section
    9). With DJ direct off, line in goes through the digital path, so EQ and
    the sub crossover apply. That adds 5–15 ms of delay, which a DJ cueing
    on headphones can hear.

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

**Standby:** the receiver goes to standby after [TBD: firmware: the
auto-standby timeout] of silence. It wakes when you play to it or press a
button.

## 6. Getting on the network

The receiver needs a network for Spotify, AirPlay, the app and updates.
Bluetooth and line in work without one.

### Ethernet (Plus, Ultra)

Plug a network cable from your router into **LAN**. The receiver gets an
address automatically. Wi-Fi setup is not needed.

### Wi-Fi

The receiver uses 2.4 GHz Wi-Fi, which nearly every home router provides.

1. **Start setup.** If it has not been set up yet, or after a factory reset,
   the receiver makes its own Wi-Fi network, named **`HiFi-…`** (for example
   `HiFi-3A7F`) [TBD: firmware: the exact name and whether it has a
   password]. The status light blinks [TBD: firmware: the colour].
2. **Join it.** On your phone or laptop, join that network. The app opens by
   itself on its **Setup** page. If it doesn't, browse to
   **http://192.168.4.1**.
3. **Connect.** Under **Wi-Fi**, press **Scan for networks**, then **Choose**
   your home network. Type its password and press **Join**.
4. **Done.** The page says **Joined …** and gives the new address. The setup
   network goes away. Rejoin your home Wi-Fi on the phone and open
   **http://hifi-lite.local** (section 6, *Finding it on the network*).

**If the page says it could not join,** the reason is shown, usually a
wrong password. Type it again and press **Join**.

**If it doesn't join:**

- **Name or password:** check them. The password is case-sensitive.
- **2.4 GHz:** if the router has separate 2.4 GHz and 5 GHz networks, pick
  the 2.4 GHz one. The receiver cannot use 5 GHz.
- **Hotel or guest Wi-Fi:** a network with a sign-in page will not work.
- **The setup network comes back** if the receiver cannot reach the saved
  network for [TBD: firmware: the fallback timeout].

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
  The `.local` address stays the same.
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
| **Bluetooth** | Put the receiver in pairing mode (below), then pair from your phone. | Remembers [TBD: firmware: number of paired devices]. Re-connects when you pick it on the phone. |
| **Tidal, Qobuz, Deezer, radio** | Play from the service's app over AirPlay. Or use a home music server (below). | |
| **Home music server** | Run Lyrion Music Server (free) on any always-on computer or NAS. The receiver finds it on its own. | Your own files, internet radio and many services. Also multi-room: several receivers in sync. |
| **Line in** | Pick **Line in** as the source (section 8). | |
| **Optical** (Ultra) | Pick **Optical** as the source. | |

**Bluetooth pairing:**

- **Lite:** hold **▶** for [TBD: firmware: the hold time].
- **Plus and Ultra:** [TBD: firmware: the pairing button].
- **From the app:** [TBD: web app: no pairing button yet].

The light blinks blue for two minutes while the receiver can be found.

**Switching source:**

- **Automatic:** the receiver switches to whatever starts playing last. A
  Spotify or AirPlay stream takes over from Bluetooth.
- **Line in and optical:** these are picked by hand, because a line input
  is always "playing". [TBD: firmware: confirm the source-priority rule]

**Hi-res:** the receiver plays up to 24-bit/96 kHz. Spotify and Bluetooth
are lower than that by nature. A home music server gets the most from it.

## 8. Controls

### Web app

Open **http://hifi-lite.local** (Plus: `hifi-plus.local`, Ultra:
`hifi-ultra.local`) in any browser on the same network.

- **Keeping it like an app:**
  - iPhone: Safari → Share → **Add to Home Screen**.
  - Android: Chrome → ⋮ → **Add to Home screen**. It is a shortcut, not
    a full install, because Chrome only installs from `https` sites, and
    a device on your home network can't have one. It works the same way.
- **What's in it:** three tabs along the bottom.
  - **Play:** what is playing and from where, previous/play/pause/next,
    the **Source**, **Volume** and mute. Also **Line in** (DJ direct or
    Through EQ) and, on Ultra, **Zone B speakers**.
  - **Sound:** the **Equaliser** and the **Subwoofers**.
  - **Setup:** **Wi-Fi**, **Name**, **Updates**, and **About and
    diagnostics**: the model, firmware, address, charger, supply voltage,
    the amplifier's state and temperature, and **Run self-test**.
- **Notices** at the top of **Play**:
  - **Speakers off: charger too weak.** The port gives too little voltage
    for the amplifier (section 5).
  - **Power limit.** The receiver has turned the sound down a little to
    stay within what the charger can give.
  - **Update failed.** The old firmware is still in place, so the
    receiver keeps working. Try again.
  - **Amplifier fault.** The amplifier switched itself off to protect the
    speakers. Check the speaker wires for a short, then press **Clear**.
- **"Looking for the receiver…"** means the app can't reach it. Check that
  it is powered and that the phone is on the same Wi-Fi.

Lyrion control apps such as Material Skin, iPeng and Squeezer also control
it when you use a Lyrion server.

### Buttons

| Lite | Press | Hold |
|---|---|---|
| **−** | volume down | [TBD: firmware] |
| **+** | volume up | [TBD: firmware] |
| **▶** | play/pause; double press: next source [TBD: firmware] | Bluetooth pairing |

| Plus | Turn | Press | Hold |
|---|---|---|---|
| **Knob** | volume | play/pause | [TBD: firmware: standby?] |
| **SOURCE** | | next source | Bluetooth pairing [TBD: firmware] |
| **PRESET** | | play preset 1 [TBD: firmware: what a preset is] | store the current stream as the preset [TBD] |

**Ultra:** the knob and three buttons are on the front panel next to the
screen. [TBD: firmware: the Ultra button map]

**Factory reset from the buttons:** see section 10.

### Remote

- **Plus and Ultra** come with a 21-key remote. [TBD: firmware: its key map;
  the remote's battery type]
- **Any tier can learn another remote,** for example the TV's, so one remote
  does both:
  1. In the app, open **Setup → Remote → Learn** [TBD: web app: not in
     the app yet].
  2. Pick a function, such as Volume +.
  3. Point the remote at the front of the receiver and press the key you
     want for it.
  4. Repeat for each function, then **Save**.
- Aim at the front panel, where the receiver's IR window is.

### Screen (Ultra)

The 2" screen shows the source, the track and artist, the volume, and the
power the charger is giving. It dims after [TBD: firmware: the dim timeout]
and lights when you touch a control.

## 9. Settings

| Setting | Where | What it does | Tiers |
|---|---|---|---|
| Name | Setup | The name in Spotify, AirPlay, Bluetooth and the `.local` address | all |
| Equaliser | Sound | 10 bands, 31 Hz to 16 kHz, ±12 dB. Presets: **Flat**, **Bass +**, **Loudness**, **Vocal**, **Treble −** | all |
| Sub outputs | Sound → Subwoofers | **On** or **Off** | all |
| Level | Sound → Subwoofers | The subwoofers' level, −15 to +10 dB | all |
| Crossover | Sound → Subwoofers | Where the subs take over the bass, 40–160 Hz | Plus, Ultra |
| Delay | Sound → Subwoofers | 0–20 ms, to line the subs up with the speakers | Plus, Ultra |
| Phase | Sound → Subwoofers | **0°** or **180°**. Pick whichever sounds fuller at your seat. | Plus, Ultra |
| Take the deep bass off the main speakers | Sound → Subwoofers | A high-pass on the mains, so they play louder cleanly | Plus, Ultra |
| Line in | Play | **DJ direct**: straight to the amplifier, no delay, no EQ. **Through EQ**: EQ and subs apply, 5–15 ms late. | Plus, Ultra (Lite is always direct) |
| Zone B speakers | Play | On/off and **Level** for the second pair | Ultra |
| Wi-Fi | Setup | Scan, join or **Forget** a network | all |
| Volume limit, balance | | [TBD: web app: not in the app yet] | all |

On Lite the sub outputs carry the full-range signal, and the app says to set
the crossover on the subwoofer itself.

**Setting up two subwoofers (Plus, Ultra):**

1. **Crossover:** set it to 80 Hz.
2. **Level:** play music with steady bass and adjust **Level** until the
   bass sounds even with the speakers.
3. **Phase:** sit in your listening seat and switch **Phase** between 0° and
   180°. Keep whichever setting gives more bass.
4. **Delay:** if a sub is much nearer or farther than the speakers, set its
   delay to the distance difference. 1 ms is about 34 cm.

## 10. Updates and factory reset

**Updates:**

- **Online:** **Setup → Updates → Check for updates**, then **Install** if
  one is offered. [TBD: firmware: where updates are published]
- **From a file:** **Setup → Updates → From a file…** and pick the `.bin`
  firmware file.
- **While it updates:** the app shows **Downloading…** then **Installing…
  do not unplug.** Keep it plugged in. It restarts by itself.
- **Fallback:** if an update is interrupted, the receiver starts on the
  previous version.

**Factory reset** erases the Wi-Fi network, the name and all sound
settings [TBD: firmware: whether it also forgets paired Bluetooth devices
and learned remote keys].

- **From the app:** **Setup → About and diagnostics → Factory reset**.
  The same card has **Restart**.
- **From the buttons:** [TBD: firmware: the combination, for example hold −
  and + together for 10 s with the power on].
- **Afterwards:** the light shows setup mode and the `HiFi-…` setup network
  comes back.

## 11. Status light

[TBD: firmware: the colours and patterns. The Ultra front board has an RGB
LED; the Lite and Plus LEDs are set by their boards.]

| Light | Meaning |
|---|---|
| off | no power, or standby with the light off |
| blinking [TBD] | setup mode: join the `HiFi-…` network |
| steady white [TBD] | on the network, ready |
| blinking blue | Bluetooth pairing |
| steady blue | Bluetooth connected |
| amber [TBD] | the charger gives less than full power |
| red [TBD] | fault: amplifier overheated, a speaker short, or the charger too weak to start the amplifier |

## 12. Troubleshooting

| Problem | Likely cause | What to do |
|---|---|---|
| Light off | No power | Try another USB-C charger and cable. A data-only cable doesn't carry power. |
| Light on, no sound at all | Amplifier off on a 5 V charger | Use a PD charger (section 5). The app says **Speakers off: charger too weak**, and **Setup → About and diagnostics** shows the **Charger**. |
| No sound from one source | Wrong source, or volume at 0 | Check the source and volume in the app. |
| No sound from one speaker | A loose wire, or the balance | Check the wires at both ends. Check **Balance**. |
| Thin bass, vague image | One speaker wired backwards | Check **+** to **+** on both. |
| Cuts out at high volume | Charger too small, or a 3 A cable on 28 V | Use a 140 W charger and a 240 W cable (Plus, Ultra) or a 65 W+ charger (Lite). |
| Sound stops after loud playing for a long time, then returns | Overheating protection | Clear the vents. Leave space above. If the app shows **Amplifier fault**, press **Clear**. |
| Hum from the subwoofer | A ground loop between the sub and another mains device | Plug the sub into the same power strip as the other gear. Try a different RCA cable. |
| Spotify doesn't list it | Different network, or a guest network | Put the phone on the same Wi-Fi. Restart the Spotify app. |
| AirPlay doesn't list it | Same as above | Same as above. |
| The `.local` address won't open | Android or Windows lacks mDNS | Use the IP address from the router (section 6). |
| Bluetooth won't pair | Not in pairing mode | Start pairing (section 7). |
| DJ deck is late against the headphones | DJ direct is off | Turn on **DJ direct** (Plus, Ultra). |
| Line in distorts | The source is too loud | Turn down the deck's master or the phone's volume to about 75%. |
| Optical: no sound | The TV is sending Dolby/DTS | Set the TV to PCM/Stereo out. |
| Nothing helps | | Factory reset (section 10), then set it up again. |

## 13. Specifications

| | Lite | Plus | Ultra |
|---|---|---|---|
| Amplifier | TI TPA3118D2, class D | TI TPA3221, class D | 2 × TI TPA3221, class D |
| Power, 4 Ω, 1% THD, per channel | 43 W | 77 W | 77 W (140 W total for 4 channels) |
| Power, 8 Ω | 23 W | 43 W | 43 W |
| DAC | Everest ES8388 | ESS ES9018K2M Sabre (mains), TI PCM5102A (subs) | same as Plus |
| Digital audio | up to 24-bit/96 kHz | same | same |
| Line in | RCA, 2 Vrms max | same, plus Everest ES7243E ADC | same, plus TOSLINK optical |
| Sub out | 2 × RCA | 2 × RCA, 2.1 Vrms | same |
| Wi-Fi | 802.11 b/g/n, 2.4 GHz | same | same |
| Ethernet | | 100BASE-TX | 100BASE-TX |
| Bluetooth | 4.2, A2DP sink, SBC | same | same |
| Streaming | Spotify Connect, AirPlay 1, Bluetooth, Lyrion (LMS) | same | same |
| Power in | USB-C PD, 20 V | USB-C PD 3.1 EPR, 28 V (20 V fallback) | same as Plus |
| Standby power | [TBD: bring-up measurement] | [TBD] | [TBD] |
| Size, weight | [TBD: enclosure] | [TBD] | [TBD] |
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
  over mDNS and has an HTTP test API. [TBD: firmware: the base path and the
  main endpoints]
  - **What it reports:** the PD contract, the input current, amplifier
    faults, clipping and overheating, the firmware version and the network.
  - **What it runs:** test tones and sweeps, and a self-test.
- **The bench scripts** that drive all of this are in the repo. [TBD: bench:
  their path and the one command to run]

## 15. Legal and licences

- **Firmware:** based on the open-source squeezelite-esp32. Its source and
  licences come with the firmware [TBD: firmware: the licence file's
  location].
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
