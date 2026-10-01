# bioshock-vr, left-handed edition

A fork of [VR-Stereo-Hub/bioshock-trilogy-vr](https://github.com/VR-Stereo-Hub/bioshock-trilogy-vr)
that makes BioShock Remastered play properly left-handed in VR, and adds the two-handed,
physical feel that VR shooters need.

- **Weapon in your left hand, plasmids in your right**, with a mirrored viewmodel so you see a
  real left hand holding the gun.
- **Jack's hands sit exactly on yours.** His palm is matched to your real palm, and the gun
  fires from the barrel you see.
- **Full arms** from your shoulders to both hands, instead of floating hands.
- **Both hands always visible.** Your off hand follows its controller instead of vanishing.
- **Two-handed grips** on any weapon. Hold the shotgun's pump or the Tommy gun's foregrip with
  your real hand, and the gun aims along both hands. Your hand rides the gun's moving parts,
  and if you keep holding through a reload you watch Jack do it.
- **A physical EVE hypo** (experimental). Draw it from your stomach (or hip), put the needle in your arm,
  pull the trigger.
- **Recoil and haptics** on every weapon, wrench hits and plasmid casts.
- **Plasmid aim calibration** for your hand, plus an aim dot for plasmids.
- **Solid HUD bars.** The health and EVE fills no longer show the world through them.

Left-handed mode is **your choice at install**: the installer asks. The BioShock 1 hand
improvements (hands on your real hands, bullets from the barrel, arms, two-handed grips, the
EVE holster) are on for everyone, and each one can be switched off in F10. Left-handed mode
works in all three games; everything else is **BioShock 1 only** for now.

---

## Install

You need **BioShock Remastered** (Steam) and a PC VR headset with a runtime the mod can use:
Virtual Desktop (VDXR), Quest Link / Air Link, or SteamVR (Index, Vive, WMR, Steam Link).

1. Download the latest `bioshock-vr-lefthand-....zip` from the
   [Releases page](https://github.com/Owloeb/bioshock-trilogy-vr-lefthand/releases) and unzip
   it anywhere.
2. Double-click **`Install.bat`**. It:
   - finds BioShock Remastered in your Steam libraries (or lets you pick the folder),
   - copies the mod into the game folder,
   - backs up any other mod's `xinput1_3.dll` it replaces (the head-tracking mod uses the same
     file; the two can't run together),
   - asks whether you're left-handed, and whether you want to walk with the right stick.
3. Start your VR runtime, then launch BioShock Remastered from Steam. **VR starts by itself.**
   With Virtual Desktop, set its OpenXR runtime to **VDXR** and launch the game from inside
   Virtual Desktop.
4. **First time only**, set a square resolution. Headset screens are close to square, so a
   16:9 image wastes most of its pixels. Press **F10** → **VR camera (M3/M4)** → **Render
   resolution**, pick **2560 x 2560**, press **Write to Bioshock.ini**, and restart the game.

To remove the mod, run **`Uninstall.bat`** from the same folder. It deletes the mod's files,
restores anything it backed up, and asks before touching your saved settings.

If VR doesn't start, or you get a flat floating screen, see `TROUBLESHOOTING.txt` in the zip
(or [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)).

### First time in the headset

All of these save themselves. You do them once.

1. **Teach the mod your relaxed off hand.** Raise any plasmid (not Telekinesis) and hold it
   still for a second.
2. **Calibrate plasmid aim.** F10 → **Decoupled aim (M6)** → **Calibrate plasmid aim**. Point
   your plasmid hand at the dot the way you'd naturally cast, and squeeze that hand's grip.
3. **Set two-handed grab points** for the guns you want to hold with both hands. F10 → **Off
   hand + two-handed grip** → **Set grab point**, hold the gun, put your off hand where the
   fore-end or pump is, and squeeze.
4. *Optional:* if the arms look too long or short, adjust them in **Arms**. If the EVE holster
   isn't where your hand naturally goes, move it in **EVE holster**.

### Controls

In left-handed mode the weapon and plasmid hands swap; the face buttons stay where they are.

- **Weapon trigger** fires. **Weapon grip** switches weapon (hold it for the wheel).
- **Plasmid trigger** casts. **Plasmid grip** switches plasmid (hold it for the wheel), or
  grabs a gun's fore-end when your hand is on it.
- **X** reloads (and hacks, and injects EVE the classic way). **A** uses, **B** jumps, **Y**
  takes a first-aid kit, the **menu button** pauses (or **X + Y** together).
- **Ammo type:** rest your thumb on the thumbrest of your move-stick hand and push the turn
  stick.
- **EVE:** reach your weapon hand to your stomach, squeeze the grip, push the needle into your
  plasmid forearm, pull the trigger.
- **Wrench:** swing it. **Recenter:** click both sticks.

The upstream controls table, further down, has the full list.

---

## The F10 menu

Press **F10** in game to open it (the mouse works on it). The sections below are in the order
they appear. Most players only ever touch a handful of these. Every section not listed here
(**VR PACING**, **Camera debug**, **Reentry probe**, the **Bones** lock options and the frame
inspector) is diagnostics: leave it alone.

**How settings save.** Everything in **Input**'s hand options, **Hands + weapon**, **Off hand +
two-handed grip**, **Arms**, **EVE holster** and plasmid calibration saves **automatically**.
Settings marked **(preset)** below are saved only when you press **Save preset values** in the
PRESET block. A few marked **(this session)** reset every launch.

### Top of the window

- **Menu text size**: make the menu readable in the headset.
- **Developer tools**: leave off. It shows probes and diagnostics.

### VR and HUD (first block)

- **VR HUD (gameswf on a floating quad)**: keep on. It puts health, EVE and ammo on a floating
  panel you can read in VR.
- **Solid HUD bars**: keep on. It makes the health and EVE fills opaque.
- **HUD distance / HUD width / HUD height offset** *(preset)*: move and size the HUD panel.
- **Hide cutscene black bars** *(preset)*: removes the letterbox bars in cutscenes.
- **During cutscenes** *(preset)*: *authored* (the default) plays cutscenes with the game's
  camera; *authored + head look* lets you look around inside them.
- **Full-screen effects across the view** *(preset)*: leave off; on, water and damage flashes
  fill the view instead of sitting on the HUD panel.

### Input

- **Smooth turn speed** *(preset)*, or **Snap turn** with **Snap angle** *(preset)*.
- **Left-handed (weapon in left hand, plasmid in right)**: the installer set this; change it
  here any time.
  - **Mirror hands + weapon (real left hand)**: shows a left hand on the gun. Keep on if you're
    left-handed.
  - **Mirror about**: keep *the gun*.
  - **Muzzle trim for this weapon**: only if a gun's muzzle flash sits off its barrel. Saved
    per weapon.
- **Swap sticks (move right, turn left)**: walk with the right stick.
- **Ammo-select modifier** *(this session)*: what you hold while pushing the turn stick to pick
  an ammo type. The default is the thumbrest on your move-stick hand.
- **Swing the wrench to attack** *(preset)* and **Swing speed needed** *(preset)*: lower the
  speed if swings don't register, raise it if the wrench fires by accident.
- **Stick deadzone** *(this session)*.

### PRESET

- **VR PRESET 1 - everything on**: turns the whole VR setup back on. Press it if something got
  switched off and you can't find it.
- **Save preset values**: saves every *(preset)* setting.
- **Auto-start VR at launch** *(preset)*: keep on.

### VR camera (M3/M4)

- **Render resolution**: see Install step 4.
- **World scale (UU per m)** *(preset)*: if the world feels too big or small. 100 is standard.
- **IPD (mm)** *(preset)*: your eye distance, for depth.
- **Head offset up / fwd** *(preset)*: raise or move your viewpoint.
- **Recenter**: also on **both stick clicks**.
- **Flat-screen crosshair** *(preset)*: off in VR (you aim with your hand).
- **Lock-on disabled** *(preset)*: keep on; the game's pad aim-assist fights hand aiming.

### Decoupled aim (M6)

- **Calibrate plasmid aim**: see "First time in the headset". **Reset** clears it.
- **calibration distance (m)**: the range where your plasmid hits exactly where you point. Set
  it to the range you usually fight at (12 m by default).
- **Aim dot for plasmids**: a dot where your plasmid will land.
- **Aim laser** *(preset)*: dots along the weapon's aim line.
- **Aim dot** with **aim dot distance** / **size** *(preset)*: a single aim dot for weapons.
- The **R / L aim trims** and **ray offsets** are per-weapon calibration. You don't need them:
  bullets follow the barrel you see, and the plasmid calibration replaces the L trims.

### Hands + weapon (M7)

- **Hands match your real hands (grip pose)**: keep on. Jack's palm sits on your palm.
  - **palm depth (cm into the fist)**: if the hand looks like it floats around the
    controller, or sinks into it.
  - **Bullets follow the gun barrel**: keep on. Shots fly where the gun you see points.
    - **barrel left/right** and **barrel down/up**: only if one gun consistently shoots to one
      side. Saved per weapon; **Default for this weapon** undoes it.
- **Recoil + haptics** and **recoil strength** *(this session)*.
- **model scale** / **WEAPON scale** *(preset)*: hand and gun size.
- The **offset** and **trim** sliders and **Save offsets** are fine-tuning for the hand model.
  Leave them at zero with hand matching on.

### Off hand + two-handed grip

- **Show the off hand**: your other hand follows its controller.
- **Two-handed grip**: grab a gun's fore-end with your off hand's grip.
- **Buzz the off hand in the grab zone**: a buzz when your hand is on the grab point.
- **grab radius (cm)**: how close your hand has to be to grab (12 by default).
- **slide-off distance (cm)**: how far your hand can slide along the gun before it lets go.
- **Watch Jack reload while you hold the grip**: hold the fore-end through a reload and Jack's
  hand does the reload.
- **Set grab point** / **Clear grab point**: per weapon (see "First time in the headset").
- **held hand rides** (per weapon): what your held hand follows. *Nearest part* rides the pump,
  slide or crank you're holding; *Gun body only* stays put on the gun; *Jack's own hand* shows
  Jack's hand instead of yours.
- **Watch Jack between shots too** (per weapon): Jack's hand does the in-between-shots action,
  like the crossbow's prime. On for the crossbow by default.

### Arms

- **Full arms (IK from the shoulders)**: arms from your shoulders to your hands.
- **shoulders down / apart / back (cm)**: where your shoulders sit, from the centre of your
  head. Adjust if the elbows bend oddly.
- **elbows out**: how far the elbows flare.
- **arm length (x Jack's)**: if the arms look too long or short.

### EVE holster

- **EVE holster**: on/off.
- **No automatic EVE injection**: a plasmid cast you can't afford just clicks, instead of the
  game injecting a hypo for you.
- **Holster on your: stomach / hip**: the stomach is the default. Each spot keeps its own position.
- **down / sideways (or out to the side) / forward (cm)**: where the holster is, from your eyes.
- **reach (cm)**: how close your hand has to be (stomach 16, hip 20).
- **injection surge (hand shake)**: the tremor as the EVE goes in; 0 turns it off.

### Body / locomotion (M7.5)

- **Body follows head**: keep on. Pushing the stick forward walks where you look.
- **Follow rate** / **Deadzone** *(preset)*: soften how the body follows your head.
- **Instant move direction** *(preset)*: keep on.

---

## What changed, and how it works

### 1. Left-handed mode (all three games)

**What you get:** the weapon trigger, grip, aim and viewmodel move to the left controller;
plasmids move to the right. The optional stick swap moves locomotion to the right stick.

**How it works:** the mod swaps controller *roles*, not bindings. The physical left controller
fills the "weapon" role and the right fills "plasmid". Every system that reads a role (aim ray,
laser, viewmodel, swing-to-attack, per-hand tuning) follows automatically. Face buttons and the
menu button stay physical, so every controller profile keeps working.

**Saved in:** `handedness.ini`. It saves on every change and loads at startup, and no VR preset
can override it.

### 2. Mirrored viewmodel (BioShock 1, experimental)

**What you get:** without the mirror, left-handed mode still shows a *right* hand holding the
gun. With it on, you see a real left hand on the weapon and a right plasmid hand, and muzzle
flashes, smoke, steam and tracers come from where you see them.

**How it works:** the game only has a right-handed rig, so the mod mirrors the finished image
rather than the skeleton:

- **Render mirror.** Viewmodel draw calls are recognised by where they're called from in the
  engine, then reflected about a plane through the gun, with triangle winding flipped. Each
  eye's plane comes from that eye's final camera, so the gun stays solid while you move.
- **Engine rig placement.** The engine's gun is placed at the mirror image of what you should
  see. The engine's muzzle and the visible muzzle are then the same point, so flashes and
  tracers leave the barrel you're looking at.
- **Effects.** Effects attached to the gun (smoke, steam, flash) are flipped with it.

**What to expect:** most guns line up out of the box. If a gun's flash sits a little off the
barrel, adjust **Muzzle trim for this weapon** in F10. It's saved per weapon in `mirror.ini`.

### 3. Hands on your real hands (BioShock 1)

**What you get:** Jack's palm sits where your palm is on the controller, at every angle, so
the hands feel like yours. Shots leave the barrel you see and fly where it points.

**How it works:**

- **Grip-pose placement.** OpenXR reports where your palm is (the grip pose). The mod measures
  Jack's palm from his finger bones and solves the rig placement that puts the two on top of
  each other. **Palm depth** fine-tunes how deep the controller sits in the fist.
- **The gun rides the controller.** The gun is placed from the weapon hand's *resting* grip,
  so when the game animates the gun (a pump, a twist, a prime) it moves on top of your
  controller instead of sliding out of your hand.
- **Bullets follow the barrel.** Shots start at the rendered muzzle and travel along the gun's
  barrel, learned from its resting pose. A few models are angled in toward the crosshair (the
  pistol and crossbow ship with their angles); **barrel left/right** and **down/up** in F10
  correct any gun, per weapon, and move the laser live.

**Saved in:** `hands.ini` (placement, palm depth, barrel aim) and `barrel.ini` (per-weapon
barrel angles).

### 4. Full arms (BioShock 1)

**What you get:** arms from your shoulders to both hands, for the weapon hand and the plasmid
hand. **Full arms** in F10 → **Arms** turns them on or off.

**How it works:** a two-bone solver bends Jack's upper arm and forearm from a shoulder point
(placed from your head, following your body's turn) to each drawn wrist. The elbow bends down
and outwards and never folds into your chest. When you roll your wrist past a comfortable
range, the elbow lifts to take the extra turn, the way a real arm does. The arm's twist bones
share out the rest. Sliders set where your shoulders are and the arm length.

**Saved in:** `arms.ini`.

### 5. Always-visible off hand (BioShock 1)

**What you get:** the hand you're not using follows its controller all the time. A free off
hand is relaxed and open. While a plasmid is raised, your empty weapon hand shows and the
holstered gun stays hidden.

**How it works:** upstream hides the unused hand. This fork places it at its own controller
every frame with the same rigid bone move as the active hand. The relaxed pose is captured
from the game's own plasmid-hand idle and saved in `offhand_neutral.ini`.

### 6. Two-handed grip (BioShock 1)

**What you get:** once a gun has a grab point, bringing your off hand near it buzzes that
controller. Squeeze the grip to take hold:

- the gun aims along the line from your back hand to your front hand;
- your off hand stays on the grip, exactly where you placed it;
- your hand **rides the part of the gun it's holding**: the shotgun's pump slides it, the
  grenade launcher's barrel turns it;
- **keep holding through a reload and you watch Jack do it**: the hand blends over to Jack's
  own reload animation on the gun, then settles back on the grip. Let go of the grip during a
  reload and the hand is yours again, so you can mime it. Automatic reloads count too (the mod
  learns each gun's magazine count from the first few shots);
- let go of the grip, or slide your hand more than 35 cm along the gun, to release.

**Per weapon**, under **held hand rides**: the part your hand follows (the nearest part by
default), **Gun body only**, or **Jack's own hand (classic)**, which copies the game's left
hand and suits the chemical thrower's wrench (its default). **Watch Jack between shots too**
shows his hand for a gun's between-shot action as well, such as the crossbow's priming pull
(on by default for the crossbow).

**How it works:** grab points are recorded relative to the weapon controller, per weapon and
per hand setup, in `twohand.ini`. The two-handed pose is applied where the mod first reads the
controllers, so the viewmodel, bullets, laser, mirror and swing detection all agree without
extra code. Every gun carries its own skeleton (`SG_Pump`, `Bone_Wrench`, `MainBarrelBone` and
so on); the grab point is attached to the chosen part at rest and follows that bone's live
motion. Inside the grab zone, your off-hand grip is kept from sending its usual "raise
plasmid" input, so reaching for the fore-end never switches you to plasmids.

**Settings:** grab radius (12 cm), slide-off distance (35 cm), the buzz, and **Watch Jack
reload while you hold the grip**. Guns without a grab point stay one-handed, deliberately.

### 7. EVE holster (BioShock 1, experimental)

**What you get:** a physical EVE hypo in a holster on your **stomach**, drawn with your weapon
hand. The holster can be moved back to the **hip** of your weapon hand in F10. The stomach is
the default because the main mod's (planned) manual reloading will use the hips for something else.

1. **Draw.** Reach your weapon hand to the holster; the controller buzzes steadily while
   you're in reach. Squeeze the grip and Jack's real EVE syringe is in your hand. You can
   squeeze on the way in: a squeeze that starts just short of the holster while your hand is
   moving toward it still draws if the hand gets there within about half a second. A squeeze
   with your hand held still nearby goes to the game as usual (the weapon wheel). If a gun was
   out, the plasmid comes up so your other arm is ready.
2. **Needle in.** Push the needle into your plasmid forearm. Both controllers buzz when it goes
   in.
3. **Inject.** Pull the trigger. The game runs its own injection: the plunger goes down, the
   EVE fills, a hypo is used. Your hand trembles as the plunger goes down and jolts as the EVE
   hits, then settles. The empty syringe stays in your hand for a moment, then goes back.

Let go of the grip before pulling the trigger and the hypo goes back unused. A holster squeeze
never reaches the game, so it can't open the weapon wheel.

**Staying out of the way at the stomach:** your weapon hand passes in front of your stomach all
the time, so there the holster ignores the hand while it's busy with the gun: two-handing it,
on the trigger, or aimed ahead (hip-fire). In those cases there's no buzz, and the squeeze goes
to the game. The stomach reach is 16 cm (the hip's is 20 cm). The log
says why when a squeeze at the stomach was left to the game (`[eve] holster: squeeze at the
stomach left to the game - ...`).

**No automatic injection:** the mod learns what each plasmid costs from your casts. A cast you
can't afford never reaches the game, so the game can't inject on its own. You get an empty
click on that hand instead. **No automatic EVE injection** in F10 switches this off.

**How it works:** the syringe is the game's own `BioAmmoHypoTool` actor. The mod shows it with
the engine's `set ... bHidden` console command and places it on the weapon hand's gun socket
every frame. That's the same spot the game hangs it from during its own injection, so when you
pull the trigger the mod presses X (the game's EVE button, with the plasmid raised) and the
game takes over without a jump. The needle test is a capsule around the forearm, from wrist to
elbow, using the arm solver. The holster hangs from your neck rather than your eyes, so looking
down at it or leaning into a fight doesn't move it away from your hand. Its height follows your
settled posture: ducking, hunching or bobbing while you move doesn't drag it down, but a crouch
you hold brings it down with you.

**Settings:** in F10 → **EVE holster**: stomach or hip, then that spot's position (down,
sideways, forward) and reach. Each spot keeps its own position, so switching back and forth
loses nothing. Also the injection surge strength (0 turns the shake off). A squeeze that misses
near the holster logs where your hand was relative to it (`[eve] holster: squeeze ... not a
draw`), which is the quickest way to tune the position.

**Still being tuned:** feedback on the stomach position, the reach and how the needle registers
is welcome.

### 8. Recoil and haptics (BioShock 1)

**What you get:**

| Weapon | Kick | Buzz |
|---|---|---|
| Shotgun | big kick up and back, slow return | heavy |
| Tommy gun | small per round, climbs a little in a burst | light, per round |
| Pistol | quick snap | sharp |
| Grenade launcher | heavy, slow return | thump |
| Crossbow | light | twang |
| Chemical thrower | a shimmer while spraying | light rumble |
| Research camera | none | shutter click |

A wrench hit thumps the weapon hand and a plasmid cast buzzes the plasmid hand. With two
hands on the gun, the kick is about half as strong and both hands move with it.

**How it works:** a shot counter at the engine's fire point drives a spring on the *drawn* gun
only. The bullets still go where the gun points, so **recoil never costs accuracy**. There's
one on/off switch and a strength slider in F10 ("Hands + weapon").

### 9. Plasmid aim (BioShock 1)

**What you get:** a one-press calibration for your plasmid hand, and an aim dot that shows
where a plasmid will go.

**Why it matters:** upstream ships a plasmid aim correction of 37° sideways and 11° down. That
is one player's left-wrist posture, and on anyone else's hand, especially a right hand, it
throws casts well off target. **Calibrate plasmid aim** replaces it with yours. It's saved per
hand setup in `aim_plasmid.ini`, and **Reset** restores the shipped value.

**Calibration distance:** you sight with your eye but the cast leaves your hand, so the two
lines cross at exactly one range: the calibration dot's. The default is 12 m, which keeps
both close and long casts within a hand's width. Set it to the range you fight at, recalibrate,
and set the aim dot distance to match.

In mirror mode the sideways aim sliders show values as you see them, so dragging right always
moves the shot right.

### 10. Solid HUD bars (all games using the HUD panel)

**What you get:** the health and EVE bar fills render solid on the VR HUD panel instead of
glowing and see-through.

**How it works:** the HUD is drawn to its own layer, and the mod rebuilds each pixel's opacity
before showing it. Upstream's rule could leave a bright red fill about 10% opaque. This fork
makes every coloured pixel at least as opaque as its brightest colour channel. There's an F10
toggle to compare with the old behaviour.

### 11. Developer tools

The investigation tooling (draw census, one-frame draw trace, hardware-watchpoint bone probe,
effects probe, mirror internals, arm diagnostics, an animation log that writes each weapon
animation and the gun's moving bones to `bioshockvr.log`, and the EVE probe used to find the
syringe, EVE level and injection) is kept but hidden. Tick **Developer
tools** at the top of F10 to show it.

---

## Settings files

All live in `%LOCALAPPDATA%\BioshockVR\`, next to the upstream files. Delete any one to reset
that feature.

| File | Holds |
|---|---|
| `handedness.ini` | left-handed, stick swap, mirror |
| `hands.ini` | hand placement, palm depth, barrel aim (plus the upstream hand offsets) |
| `barrel.ini` | per-weapon barrel angles |
| `arms.ini` | arms on/off, shoulder position, arm length |
| `twohand.ini` | off-hand and grip settings, grab points, what the held hand rides, per weapon |
| `mirror.ini` | per-weapon muzzle trims (mirror mode) |
| `aim_plasmid.ini` | plasmid aim calibration, calibration distance, plasmid aim dot |
| `eve.ini` | EVE holster on/off, automatic-injection guard, stomach or hip, each spot's position and reach, injection surge |
| `offhand_neutral.ini` | the relaxed off-hand pose |
| `overlay.ini` | F10 text size, Developer tools |
| `vrpreset.ini` | everything marked *(preset)* in the F10 guide: HUD panel, turning, swing, camera scale, aim dot, laser (written by **Save preset values**) |
| `weapons.ini` | per-weapon aim profiles (upstream; written by **Save preset values**) |

## Known limits

- **Plasmid effects sit slightly off the hand** in mirror mode. They hang off the game's own
  copy of the hand, which the mirror can only line up with the hand you see at one point.
- **The EVE holster is experimental.**
  - **Hypo count:** the mod learns where it's stored over your first two injections. Until
    then it hands you a hypo even if you have none; the game then refuses the injection and
    the syringe goes back.
  - **First draw:** if the mod hasn't found the syringe yet, the first draw of a session
    injects straight away, the same as pressing X. That finds it, and every draw after that
    is the full physical version.
  - **Mirror mode:** the syringe's label reads backwards.
- **Telekinesis is aimed by your head.** It skips the aiming code the mod hooks for every other
  weapon and plasmid, so it can't be pointed with your hand yet.
- **Grab points belong to a hand-placement mode.** Switching **Hands match your real hands** on
  or off uses a separate set of grab points, so each mode keeps its own.
- **A gun whose magazine count can't be learned** (the chemical thrower's fuel, for one) shows
  Jack's hand only for reloads you start yourself.
- **Mirror mode can need a per-gun muzzle trim** where a gun's flash isn't attached to the
  barrel.
- **Tested by one player on one setup.** Controller profiles other than Quest Touch get the
  same bindings (haptics included), but haven't been tried on hardware.

## Branches

| Branch | What it is |
|---|---|
| `lefthand` (default) | everything, plus this README |
| `arms` | where the arms and hand-placement work was developed (merged into `lefthand`) |
| `eve` | where the EVE holster was developed (merged into `lefthand`) |
| `pr/left-handed` | left-handed mode only, proposed upstream |
| `pr/mirror-viewmodel` | adds the mirrored viewmodel |
| `pr/hands-haptics` | adds the off hand, grips, recoil and haptics, plasmid aim and HUD bars |

## Building from source and making a release

You only need this to change the code. Install Visual Studio 2022 Build Tools with the C++
workload and the "C++ CMake tools" component, then in PowerShell:

```powershell
git clone --recursive https://github.com/Owloeb/bioshock-trilogy-vr-lefthand.git
cd bioshock-trilogy-vr-lefthand
Set-ExecutionPolicy -Scope Process Bypass
.\tools\build.ps1 -Release
.\tools\install.ps1 -Release
```

`install.ps1` finds the game in your Steam libraries; pass `-GamePath` to point it elsewhere.
`.\tools\package.ps1` builds the same zip a release ships, into `dist\`.

**Releases are built by GitHub.** Pushing a tag that starts with `v` runs
`.github/workflows/release.yml` on GitHub's Windows machines: it builds the mod, packages the
zip (installer included) and publishes it on the Releases page with
`release/RELEASE-NOTES-lefthand.md` as the description. Update that file first, then:

```powershell
git tag v0.8.3-lh.2
git push fork v0.8.3-lh.2
```

## Credits

- **[VR-Stereo-Hub/bioshock-trilogy-vr](https://github.com/VR-Stereo-Hub/bioshock-trilogy-vr)**,
  the mod this is built on. All the hard parts (stereo, head tracking, the bone drive, the aim
  hooks) are theirs.
- **BioVRDev's BioVR**, whose design the always-visible off hand and two-handed grip are
  modelled on: the grab and release distances, the grab-zone buzz, and aiming along both hands.
- Built with help from Claude (Anthropic).

---
---

# The upstream mod: bioshock-vr

A native VR mod for **BioShock Remastered**, **BioShock 2 Remastered** and **BioShock
Infinite** (PC, Steam): stereoscopic rendering, 6DOF head tracking, and motion controllers -
weapons in one hand, plasmids/vigors in the other - targeting Quest 3 via Virtual Desktop
(VDXR/OpenXR), any other OpenXR runtime with a **32-bit** loader path, and **SteamVR /
Steam Link through the bundled compatibility shim** (SteamVR has no 32-bit OpenXR runtime
of its own; the zip ships `bvr_steamvr32.dll` + `openvr_api.dll` and the mod falls back to
them automatically - covers Index, Vive, WMR and Steam Link; see
[docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)).

One zip serves all three games - the same DLL set adapts to whichever game it is dropped
into:

| game | install folder (under `steamapps\common\`) | status |
|---|---|---|
| BioShock Remastered | `BioShock Remastered\Build\Final\` | playable, tuned |
| BioShock 2 Remastered | `BioShock 2 Remastered\Build\Final\` | playable, tuned |
| BioShock Infinite | `BioShock Infinite\Binaries\Win32\` | **early access** - playable start to current test point; see the release notes for known issues |

The mod is a DLL injected into the game's process. It hooks the game's DirectX 11 renderer and
the Vengeance engine (Unreal Engine 2.5 lineage) camera path, and drives them from an OpenXR
session. No game files are modified and no game assets are distributed.

> **Status:** playable. Working today: full-rate stereo rendering, 6DOF head tracking,
> motion-controller aim with a laser (right hand = weapons, left hand = plasmids), the visible
> viewmodel following the controller with the inactive hand hidden, **per-weapon aim profiles**
> (every weapon keeps its own laser calibration and swaps it in the moment you equip it),
> body-follows-head movement ("walk where you look"), the game HUD (health/EVE/ammo - and the
> pause menu) on a readable floating panel in VR, VR-standard controller bindings with
> ammo-select on the right stick, a single-eye desktop mirror, and in-headset tuning sliders
> that persist. The shipped defaults ARE a full calibration (aim trims, per-weapon profiles,
> body follow) tuned in-headset on a Quest 3. See [docs/STATUS.md](docs/STATUS.md) for the
> current state and [docs/ROADMAP.md](docs/ROADMAP.md) for what is next.

## Requirements

- BioShock Remastered on Steam (`steamapps\common\BioShock Remastered`)
- A PCVR-capable headset. Primary target: Meta Quest 3 with Virtual Desktop (VDXR); Meta
  Link/Air Link (Oculus runtime) also ships a 32-bit runtime. Any OpenXR runtime with a
  **32-bit** loader path works natively; **SteamVR-only setups (Index, Vive, WMR, Steam
  Link) work through the bundled shim** - install all four DLLs and it engages
  automatically ([docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) has the details;
  Quest+VDXR stays the most-tested lane, WMR bindings are unverified on hardware)

## Install (release zip)

1. Download the release zip and copy **the DLLs** (`xinput1_3.dll`, `bioshockvr.dll`, and -
   for SteamVR setups - `bvr_steamvr32.dll` + `openvr_api.dll`) into the game's binary folder:
   `...\steamapps\common\BioShock Remastered\Build\Final\`
2. If you use **itsloopyo's head-tracking mod**, remove or back up its `xinput1_3.dll` first -
   the two mods use the same injection vector and cannot coexist.
3. Headset side (Quest 3 + Virtual Desktop): in Virtual Desktop's Streaming tab set the OpenXR
   runtime to **VDXR**, connect, then launch the game from Steam inside Virtual Desktop.
   (On Steam Link or a SteamVR-native headset just launch with SteamVR running - the mod
   falls back to the bundled SteamVR shim by itself; see
   [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) if anything reports "no OpenXR runtime".)
   **Set the game's resolution to roughly SQUARE, not 16:9** - something like 2700x2700.
   The mod sizes the eye render target from the game's backbuffer, and headset panels are
   near square, so a 16:9 backbuffer renders a wide strip that the headset then throws
   away. At 3840x2160 on a Quest 2 only ~54% of the width is inside the FOV: a square
   2750x2850 has *fewer* total pixels, is sharper in the headset, and runs faster. The
   startup log prints the consequence - `xr: headset fov half-angles ... -> game hfov N deg
   (aspect A)`; the closer `aspect` is to 1.0, the less you are wasting.
4. Launch the game through Steam. The mod logs to `%LOCALAPPDATA%\BioshockVR\bioshockvr.log`.

To uninstall, delete the mod's DLLs (restore itsloopyo's backup if you made one).

### Troubleshooting

**[docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)** covers every "no VR / flat mode"
report received so far: the 32-bit OpenXR registry key (and how to fix it per runtime),
broken 32-bit API layers (ReShade and friends - and OBS, which several users had to
close or remove), the bundled SteamVR shim (how the automatic fallback works, its log,
the `xr.ini` override, controller coverage), the `XR_RUNTIME_JSON` override, and
per-game resolution/windowed-mode guidance. The release zip ships the same text as
`TROUBLESHOOTING.txt`.

Two symptoms are easy to confuse, so start by telling them apart: if the log shows **no
OpenXR runtime**, that is the runtime/registry side above. If VR starts fine but the game
is **a flat floating screen with no head tracking**, that is a saved `vrpreset.ini` with
stereo and head-drive switched off - copy the matching `preset-*` file from the zip over
yours, or arm it in F10 (see the "flat floating screen" section).

### If it crashes or misbehaves: clear your settings first

The mod's saved settings live in **`%LOCALAPPDATA%\BioshockVR\`** (paste that into the Explorer
address bar). Close the game, then delete or move `vrpreset.ini`, `hands.ini`, `weapons.ini` and
`command.txt` if present, and relaunch.

Those files override the built-in defaults **key by key**, so a value written by an older version -
or one saved mid-experiment - keeps applying even after an update fixes the default. Clearing them
puts you back on the shipped defaults, which are the ones that get tested. You only lose your own
tuning, and pressing **VR PRESET 1** restores a working configuration immediately.

Keep `bioshockvr.log` from the same folder if you want to report the problem; crash dumps, when
there are any, land in `%LOCALAPPDATA%\BioshockVR\crash\`.

## Playing in VR

1. Launch the game - **VR arms itself automatically** (your saved settings load and the
   full stack - pacing, 6DOF camera, motion controllers, aim + laser, viewmodel drive,
   body-follows-head, stereo - comes up with no F10 trip). To opt out: untick
   "Auto-start VR at launch" in the F10 preset block and save, or set `autoVr=0`
   in `vrpreset.ini` with the game closed.
2. The **F10** overlay is still there for tuning, and **VR PRESET 1** (BS1) / **APPLY
   PRESET** (BS2) re-arms everything on demand. No restart is ever needed - the mod
   answers the game's one-shot startup gamepad check itself, so the motion controllers
   engage immediately, first launch included.
3. **Click both thumbsticks together to reset the view** (same as the F10 recenter
   button) - do it standing in your neutral pose, facing forward.
4. Quest 3 Touch controls:

| Input | Action |
|---|---|
| **Swing your right hand** | **swing the wrench** - only while the wrench is equipped; the trigger still works too |
| Right trigger | fire weapon (first pull raises it) |
| Left trigger | cast plasmid (first pull raises it) |
| Right grip | switch/cycle weapon (hold for the radial) |
| Left grip | switch/cycle plasmid (hold for the radial) |
| Left stick | move (crouch on click) |
| Right stick | turn; **hold the LEFT thumbrest + push up/down/left = select ammo type** (zoom is removed in VR) |
| Left thumbrest | ammo-select modifier (the pad above the X/Y buttons - just rest your thumb on it) |
| A | use / interact (and menu confirm) |
| B | jump |
| X | reload / hack / inject EVE |
| Y | first-aid kit |
| Left menu button | pause (hold: map/objectives) |
| **X + Y together** | same as the menu button (use it when Steam Link's overlay eats the real one) |

   Under VR the right stick no longer pitches the view (your head does); `vrinput pitchkill
   off` restores stick pitch if you want it back.

   **Ammo select** used to mean holding the right stick *click* while pushing that same
   stick, which is awkward. It is now the **left thumbrest** - the smooth pad above the
   X/Y buttons, which senses your thumb resting on it. It has to be the left one: your
   right thumb cannot rest on the right pad and push the right stick at the same time.
   The overlay's "Ammo-select modifier" combo (or `vrinput ammomod click|thumbrest|both`)
   switches back to the stick click, or accepts either. Controllers with no thumbrest
   (Pico, some SteamVR setups) keep the stick click automatically.

Tuning (all in the overlay, all persisted by **"Save preset values"** / `vrpreset save`):

- **World scale / IPD / game FOV** - comfort and scale calibration
- **Per-hand aim trim** (up to +-90 deg) - laser/bullet direction alignment per hand
- **Per-weapon profiles** - the right hand's aim trim + ray offsets automatically follow the
  EQUIPPED weapon: tune with a weapon up and only that weapon's profile changes, swapped in
  the moment you switch. Calibration flow: fire at a wall, nudge the sliders until the laser
  sits on the bullet holes, next weapon, then one "Save preset values". The overlay's
  "weapon profile:" line shows which weapon you are editing.
- **Per-hand ray offsets** - the "Ray offset hand: L / R" selector + three sliders move the
  laser (and the bullets with it - they are one ray) to line up with the controller and model
- **Head anchor offsets** - if the camera sits wrong in the body
- **Per-hand model offsets** - the "Tuning hand: L / R" selector picks which hand the six
  position/rotation sliders edit, so the pistol and the plasmid hand are tuned independently
- **Turn controls** - "Smooth turn speed" scales the stick turn rate; "Snap turn" replaces
  smooth turning with discrete steps (angle slider, default 45 deg)
- **Cinematics** - scripted scenes (the bathysphere descent), the hack minigame, loading
  screens and FMVs are auto-detected. Cutscenes play as a full stereo projection with
  head-look by default; untick "Cinematics as stereo projection" to watch them on a big
  virtual screen instead. Flat 2D screens (hacking, loading) always use the readable screen.
- **Cutscene black bars are gone** ("Hide cutscene black bars", on by default). The game's
  widescreen bars are a flash sprite drawn over the full picture, so hiding them reveals the
  image that was always underneath - nothing is cropped, stretched or lost.
- **What the rig does during a cutscene** - the "During cutscenes" dropdown:
  *authored* (default) plays the director's camera and the authored hand animation exactly as
  the flat game does; *authored + head look* keeps the choreography but lets you look around;
  *off* leaves your head and hands driving straight through the scene.
- **Cutscene subtitles** ride the head-locked panel so they stay readable in stereo. The
  "Cutscene subtitles in-frame" checkbox puts them back in the world if you prefer.
- **Swing to attack** ("Swing the wrench to attack", on by default) - a fast right-hand motion
  swings the wrench, in addition to the trigger. Only while the wrench is equipped: the gesture
  composes a trigger pull, and a trigger pull with a gun in hand is a shot, so it is gated on
  what you are actually holding. "Swing speed needed" (3.6 m/s) is the bar your hand has to
  clear - lower it if swings are being missed, raise it if ordinary movement triggers one. Note
  it changes *when* the attack fires, not where it lands: the game aims melee from your view, so
  a sideways swing while you look forward still hits forward, exactly as the trigger always did.
- **Aim dot** ("Aim dot", off by default) - a single dot on the ray the bullet actually uses,
  not a reconstruction of it, so where the dot sits is where the shot goes. Set "aim dot
  distance" to roughly your calibration wall's distance before tuning: a dot and a bullet hole
  only line up in stereo when they are at the same depth.
- **`vrbody off`** - live A/B for the body-follows-head transfer (instant 1:1 by default)

### The bundled preset (v0.3.0+)

**A fresh install needs no tuning**: the shipped defaults are a complete in-headset
calibration (left/plasmid hand trim, per-weapon profiles for all eight holdables, body
follow, HUD placement). Just install and press VR PRESET 1.

The release zip also carries the same calibration as plain files (`vrpreset.ini`,
`hands.ini`, `weapons.ini`):

- **New users**: nothing to do - the DLL defaults are identical to these files.
- **Existing users with their own tuning**: your files in `%LOCALAPPDATA%\BioshockVR\`
  ALWAYS win over the built-in defaults, key by key - updating the DLLs changes nothing you
  tuned. To adopt the bundled calibration instead, back up and delete (or overwrite) those
  three files in `%LOCALAPPDATA%\BioshockVR\` and restart the game. To adopt only parts
  (say, the weapon profiles but not your world scale), copy just that one file - or even
  single lines: every `key=value` line stands alone.

The flat-screen crosshair is hidden by default (the laser replaces it); the "Flat-screen
crosshair" checkbox or `vrxhair on` brings it back.

The game HUD (health, EVE, ammo - and the pause menu) shows on a head-locked floating panel
during stereo gameplay; "HUD distance/width/height offset" sliders place it, `vrhud off`
disables the capture entirely, and the inactive hand's model is hidden while the other hand
is raised (`vrhands hideinactive off` shows both).

The desktop window mirrors the **left eye** while stereo runs (`vrmirror off` restores the raw
alternating view), and the game keeps running at full speed on the monitor when you take the
headset off (`vrpace off` restores the old blocking behavior).

## Build from source

```powershell
git clone --recursive https://github.com/mohamad-balouza/bioshock-vr
cd bioshock-vr
.\tools\build.ps1            # Debug build (finds the VS-bundled CMake automatically)
.\tools\build.ps1 -Release   # Release build
.\tools\install.ps1          # copies the mod DLLs into the game's Build\Final folder
.\tools\uninstall.ps1        # removes them (restores anything it backed up)
```

Building needs Visual Studio 2022 Build Tools with the **x86** MSVC toolset (the game is
32-bit) and git. `.\tools\tail-log.ps1` follows the log live.

## Legal

This project is not affiliated with, endorsed by, or connected to 2K Games, Take-Two
Interactive, or any of their subsidiaries. It distributes no game assets, no decompiled game
code, and no copyrighted material - only original injection code. A legitimately owned copy of
BioShock Remastered is required. Free and open source, forever.

## Credits

- [itsloopyo/bioshock-remastered-headtracking](https://github.com/itsloopyo/bioshock-remastered-headtracking)
  (MIT) - pioneered the `xinput1_3.dll` injection vector and the `PlayerCalcView` camera hook
  technique on this exact game; this project ports and extends those techniques.
- [praydog/REFramework](https://github.com/praydog/REFramework) (MIT) - reference implementation
  for OpenXR/D3D11 VR integration in a closed-source engine.
- **[BioVRDev/Bioshock-Remastered-VR](https://github.com/BioVRDev/Bioshock-Remastered-VR)** - a
  parallel native VR mod for the same game, and a genuinely friendly one. The two projects have
  swapped findings in both directions: their README credits this one for the
  reticle-via-console-Exec technique, the arm bone indices and the render-target HUD capture,
  and their author has given explicit permission to reuse their code and concepts here. Ideas
  taken from them so far: rendering at a near-square resolution matched to the headset panel
  instead of widening the game's FOV, the "report the game's own symmetric FOV to the
  compositor, never the headset's canted one" invariant, per-feature build guarding that logs
  and stands down instead of trusting an address, and the startup config echo block. Files that
  adapt their code carry an attribution comment naming the source. Worth a look, and worth
  trying if this mod does not suit your setup.
- Third-party libraries: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

[MIT](LICENSE)
