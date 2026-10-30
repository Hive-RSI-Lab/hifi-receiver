# Quick start

*hifi-receiver: Lite, Plus and Ultra. This is the printed sheet that goes in
the box. The full manual is [`user-manual.md`](user-manual.md).*

## In the box

- The receiver
- This sheet
- **Plus and Ultra:** an IR remote
- **"With charger" editions:** a USB-C charger. Lite comes with a 65 W charger.
  Plus and Ultra come with a 140 W charger and a 240 W (5 A) USB-C cable.

You supply your own speakers, speaker wire, and RCA cables for subwoofers or
a DJ deck.

## 1. Connect the speakers. Do this first, with the power unplugged.

- Connect **Left** to the left speaker and **Right** to the right speaker.
- Red (**+**) goes to red, and black (**−**) goes to black.
- **Lite:** push a 4 mm banana plug into each socket (single plugs: the
  sockets are too close for a dual plug).
- **Plus, Ultra:** use 4 mm banana plugs (single or dual), spades, or bare
  wire under the binding post's nut.
- No stray strand may touch the other terminal or the case.
- Use **one speaker per terminal pair, 4 Ω or more.** Never wire two speakers
  to the same terminals.

**Optional connections:**

- **Subwoofers:** powered subwoofers go to **SUB OUT** (RCA; on Plus and
  Ultra, **SUB 1** and **SUB 2**).
- **A DJ deck, phone or turntable preamp:** goes to **LINE IN** (RCA).

In each RCA pair the upper jack is left (white) and the lower one is right
(red).

## 2. Plug in the power

Plug the USB-C charger into the **USB-C** port at the back.

| Tier | Charger | What you get |
|---|---|---|
| Lite | any USB-C PD laptop charger, 65 W or more (20 V) | full power |
| Plus, Ultra | a **140 W PD 3.1** charger and a **240 W (5 A)** cable | full power |
| Plus, Ultra | a 65–100 W charger | it plays, at about half power |
| any | a phone charger or laptop port | it starts, but the amplifier stays off |

## 3. Get it on your network

- **Ethernet (Plus, Ultra):** plug in a network cable. That's all.
- **Wi-Fi:**
  1. The status light blinks. On your phone, join the receiver's own Wi-Fi
     network, **`hifi-lite-…`**, with the password **`squeezelite`**.
  2. The app opens on **Setup**. If it doesn't, browse to
     **http://192.168.4.1**.
  3. Press **Scan for networks**, **Choose** your home Wi-Fi, type its
     password, and press **Join**.
  4. The app says **Joined …**. Put your phone back on your home Wi-Fi.

## 4. Play

- **Spotify:** open Spotify, tap the devices icon, and choose the receiver
  (**hifi-lite** unless you renamed it). This needs Spotify Premium.
- **AirPlay (iPhone, iPad, Mac):** in Control Centre, tap AirPlay and choose
  the receiver.
- **Bluetooth:** in your phone's Bluetooth settings, pick **hifi-lite**. No
  pairing button is needed: it can be found whenever no other phone is
  connected to it.
- **LINE IN:** in the app, choose **Play → Input → DJ direct**. Choose
  **Streaming** to go back.

## 5. Control it

- **App:** open **http://hifi-lite.local** (Plus: `hifi-plus.local`, Ultra:
  `hifi-ultra.local`) on any phone or computer on the same network. Choose **Add to Home Screen** to keep it like an app.
- **Buttons:**
  - Lite: **−**, **+** and **▶** on top. Press **−**/**+** for volume and
    **▶** to play or pause. Hold **−**/**+** for 1 s for the previous or
    next track, and **▶** for standby.
  - Plus and Ultra: turn the knob for volume, and press it to play or pause.
- **Remote:** Plus and Ultra come with one. Any tier can be taught your TV
  remote in **Setup → Remote** once its firmware supports it (manual,
  section 8).

**Start with the volume low.** The Plus and Ultra put out up to 77 W per
channel. That is enough to damage speakers if you turn it straight up.

## Help

| Problem | What to do |
|---|---|
| No sound | Check **Play → Input** in the app (**Streaming** for Spotify, AirPlay and Bluetooth). Check the volume. Check the speaker wires are firm. |
| Plays, then cuts out at high volume | The charger is too small. Use the charger in the table above. |
| The `.local` address won't open | Android or Windows: open the receiver by its IP address. Your router lists it as `hifi-lite` (or `hifi-plus`, `hifi-ultra`). |
| Start again | In the app: **Setup → About and diagnostics → Factory reset**. See the manual, section 10. |
