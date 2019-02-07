TVP's fork of CasparCG
======================
This is fork of 2.06 Stable master branch with following fixes and enhacements which are not included in main distribution:
1. Long-GOP files are now precisely seeked. Improved H.264 decoding performance.
2. When interlaced image is paused (assuming that displayed fields comes from different moments in time), there is no image trembling (only first field is displayed).
3. Server process can't be closed with Ctrl-C.
4. Fix for IMX MPEG PAL (e.g. Sony MXF) files, so 32 VBI lines above active video lines are not displayed anymore.
5. Added `FIELD_ORDER_INVERTED` parameter when playing file, it allows fix such a file. Also available as layer CALL (like LOOP), efects immediately.
6. Decklink producer auto-detects input signal format on supported cards and scale it to channel format (works only with the same framerate formats).
7. Added new transition type: SQUEEZE (both background and foreground layers).
8. Added transition pause at half of its duration of specified length.
9. Allows creating a input channel with no initial consumer, but with decklink or NDI input open (to record it with ADD FILE, CAPTURE or instantly play with ROUTE command).
10. Added deck control, with new CAPTURE and RECORDER commands (see wiki).
11. Added native Newtek NDI (http://ndi.newtek.com) consumer able to stream with or without alpha channel.
12. Added NDI producer to play a NDI source (can be added using AMCP command or from casparcg.config).
13. It's possible to produce perfectly stable CBR MPEG-TS UDP stream. Refer to wiki page if it's required here: https://github.com/jaskie/Server/wiki/Creating-perfectly-stable-CBR-MPEG-transport-stream.

--------------------------------------
|        Original readme below       |
--------------------------------------

CasparCG Server 2.0.6 Stable 
============================

Thank you for your interest in CasparCG Server, a professional software used to
play out and record professional graphics, audio and video to multiple outputs.
It has been in 24/7 broadcast production since 2006.

This release is considered tested and stable, and is intended for use in
professional production.

More information is available at http://casparcg.com/


SYSTEM REQUIREMENTS
-------------------

1. Intel processor capable of using SSSE3 instructions. 
   Please refer to
   http://en.wikipedia.org/wiki/SSSE3 for a complete list.
   While AMD processors probably work, CasparCG Server has only been tested on
   Intel processors.

2. Windows 7 (64-bit) strongly recommended.
   CasparCG Server has also been used successfully on Windows 7 (32-bit) 
   and Windows XP SP2 (32-bit only.)
   NOT SUPPORTED: Windows 8, Windows 2003 Server and Windows Vista.

3. A graphics card (GPU) capable of OpenGL 3.0 is required.
   We strongly recommend that you use a separate graphics card, and avoid
   using the built-in GPU that exists in many CPUs, since your performance
   will suffer.

4. Microsoft Visual C++ 2010 Redistributable Package must be installed.
   See link in the Installation section below.
   
5. Microsoft .NET Framework 4.0 or later must be installed.
   See link in the Installation section below.
   
6. Windows' "Aero" theme and "ClearType" font smoothing must be disabled
   as they have been known to interfere with transparency in Flash templates,
   and can also cause problems with Vsync when outputting to computer screens.

The latest system recommendations are available at:
http://casparcg.com/wiki/CasparCG_Server#System_Requirements


INSTALLATION
------------

1. Check that your system meets the requirements above.

2. Unzip and place the "CasparCG Server 2.0.6" folder anywhere you like.

3. Install "Microsoft Visual C++ 2010 Redistributable Package" from
   http://www.microsoft.com/download/en/details.aspx?id=5555
   
4. Install "Microsoft .NET Framework" (version 4.0 or later) from
   http://go.microsoft.com/fwlink/?LinkId=225702

5. Make sure you turn off Windows' "Aero Theme" and "ClearType" font smoothing
   as they can interfere with CasparCG Server's OpenGL features!



INSTALLATION OF ADDITIONAL NON-GPL SOFTWARE
-------------------------------------------

- For Flash template support:
  1. Uninstall any previous version of the Adobe Flash Player using this file:
     http://download.macromedia.com/get/flashplayer/current/support/uninstall_flash_player.exe
  2. Download and unpack
     http://download.macromedia.com/pub/flashplayer/installers/archive/fp_11.8.800.94_archive.zip
  3. Install Adobe Flash Player 11.8.800.94 from the unpacked archive:
     fp_11.8.800.94_archive\11_8_r800_94\flashplayer11_8r800_94_winax.exe


- For NewTek iVGA support, please download and install the following driver:
  http://new.tk/NetworkSendRedist


CONFIGURATION
-------------

1. Start the "CasparCG_Server.exe" program and configure the settings. Then 
   Click the "Restart" button to save the configuration and restart
   the CasparCG Server software.

2. Connect to the Server from a client software, such as the "CasparCG Client"
   which is available as a separate download.




DOCUMENTATION
-------------

The most up-to-date documentation is always available at
http://casparcg.com/wiki/

Ask questions in the forum: http://casparcg.com/forum/



LICENSING
---------

CasparCG is distributed under the GNU General Public License GPLv3 or
higher, please see LICENSE.TXT for details.

The included software is provided as-is by Sveriges Televison AB.
More information is available at http://casparcg.com/
