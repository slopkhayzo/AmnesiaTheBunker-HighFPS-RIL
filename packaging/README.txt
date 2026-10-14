AmnesiaTheBunker.FPSFix
=======================

High-frame-rate fix for Amnesia: The Bunker (64-bit Windows).

Amnesia: The Bunker simulates the game at the rate of its Framerate option,
120 at most, and normally draws at the same rate. This fix lets the game draw
faster than it simulates and smooths what you see in between simulation
steps, without speeding up physics, scripts, AI, animation, or audio.

What it smooths
---------------
- Camera movement, including scripted cameras
- Mouse look, shown at the frame rate instead of the simulation rate
- The first-person body, arms, weapons, lantern and other held items
- Props, doors, drawers, thrown and carried objects, lights and shadows
- Creature, character and animated-prop animation
- Particles such as smoke, sparks, fire and dust
- Menu, pause-menu and inventory mouse cursors, including dragged items
- Optionally, frame times: frames paced evenly around each simulation step,
  at some cost in frame rate

Requirements
------------
- Amnesia: The Bunker for Windows. Developed and tested on the GOG build
  only. At startup the fix checks the game code it depends on and switches
  itself off if that code does not match. Other builds, such as Steam, work
  only if their code is the same; this has not been tested.
- An x64 ASI loader. It is not included in this archive.

Installation
------------
Place AmnesiaTheBunker.FPSFix.asi and AmnesiaTheBunker.FPSFix.ini beside
AmnesiaTheBunker.exe.

The fix holds the game's simulation at 60 Hz (GameplayRateOverride = 60),
whatever the in-game Framerate option says, and draws as many frames in
between as your display allows. This is how it was developed and tested. Set
GameplayRateOverride = 0 to keep the rate chosen by the Framerate option; at
120 Hz a simulation step leaves room for only one or two frames, so the frame
rate then stays between 120 and 240.

Settings
--------
AmnesiaTheBunker.FPSFix.ini lists every feature with a short description.
Set Enabled = false under [General] to switch the fix off without removing
it.

The built-in frame limiter is on and follows your display's refresh rate;
set TargetFPS to choose another rate. To use VSync or an external limiter
instead, set PresentationLimiter = false.

Frame pacing (optional, off by default): the frame that runs a simulation
step takes longer than the frames between steps, so frame times are uneven
and fast camera turns can look slightly juddery. FramePacingEvenness
(0..100, under [Timing]) evens them out at the cost of frame rate: higher
values are smoother and slower, 100 makes every frame the same length. 50
is a good place to start. It needs the built-in limiter.

Not covered
-----------
- Ragdolls and time-based screen effects (image trails, blur) still update
  at the simulation rate.
- Ropes, beams, light glows and ambient dust still update at the simulation
  rate, as do the colour and size of particles.
- Particles attached to something you carry follow it at the simulation
  rate.
- The cursor on in-world screens such as terminals is not smoothed.
- Changing the Framerate option while a game is running is untested.
- The game has not been played to the end with this version.
- Gamepads, mods and custom stories are untested.

Troubleshooting
---------------
AmnesiaTheBunker.FPSFix.log is written beside the plugin and replaced on each
launch; the previous session is kept as
AmnesiaTheBunker.FPSFix.previous.log. If the game folder is not writable, the
log goes to %LOCALAPPDATA%\AmnesiaTheBunker.FPSFix instead. If the fix
switches something off, the reason is in the log. Set Diagnostics = true in
the INI before reproducing a problem you want to report.

Removal
-------
Remove AmnesiaTheBunker.FPSFix.asi, AmnesiaTheBunker.FPSFix.ini,
AmnesiaTheBunker.FPSFix.log, and AmnesiaTheBunker.FPSFix.previous.log. Do not
remove a shared ASI loader unless no other installed plugin depends on it.
The game's own files and save data are not modified.

Misc
-------
For more game fixes, visit https://slop-blog.enkhayzomachines.net/fixes :)
