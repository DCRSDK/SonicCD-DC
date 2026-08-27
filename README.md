# **Sonic CD 2011 for SEGA Dreamcast**
A Sonic CD/RSDKv3 port for SEGA Dreamcast based on the RSDKv3 Decompilation, using the mobile version as base.
  
+ Requires ffmpeg and python3 for asset generation, as well as existing KallistiOS and mkdcdisc install for compiling the .elf and building the CDI.
+ Build in Linux or MacOS environments, or Windows via WSL (Debian or Ubuntu based preferably)

+ Build from your own KallistiOS setup by cloning this git and following the instructions in DC_Build_Guide.md
+ Bring your own Sonic CD Data.rsdk (MUST be mobile version, PC 2011 and Origins are untested, and presumed unsupported), as well as FMV files (these can be sourced from anywhere, so long as you have seperate ones for US and JP soundtracks, encoded into mp4 DC_Build_Guide.md details what you should be naming them as).
NO ASSETS ARE PROVIDED IN THIS RELEASE.

What Works:
+ Sonic CD full game including Special Stages
+ JP and US soundtracks
+ Opening and Ending FMVs
+ Time Attack (though you cannot upload times)
+ D.A. Garden, level select and sound test (must be unlocked through play to access from main menu)
+ Saves (including custom icon in the Dreamcast BIOS save menu)

What doesn't work:
+ Leaderboards Menu
+ Achievements Menu

Dev menu is enabled by default and is accessed by pressing the Y button. When selecting stages in the Dev menu press Start button for regular play, or the A button for debug mode. This can be changed by toggling it off in the settings.ini in the /dreamcast directory before committing to CDI if you so wish.


**Special Thanks**
+ Rubberduckycooly & everyone involved with the RSDKv3 decompilation - without that none of this would have been possible
+ SF94 & everyone involved with the Sonic Mania Dreamcast port - this port was largely inspired by it, you guys rock!



**Notice regarding AI use:**

This was almost entirely created using Claude AI. I do not consider myself a coder, more a project director. If you have any views regarding AI use that would otherwise prevent you from trying this port out, that's fine. Move along and enjoy your day.

Sonic CD (C) SEGA
