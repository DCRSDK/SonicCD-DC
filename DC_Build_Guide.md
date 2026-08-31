Before you start - Here are the filenames you should be naming your FMV mp4 files:


| File Name | Description |
| :--- | :--- |
| `Opening.mp4` | Opening, JP |
| `OpeningUS.mp4` | Opening, US |
| `Good_Ending.mp4` | Good Ending, JP |
| `Good_EndingUS.mp4` | Good Ending, US |
| `Bad_Ending.mp4` | Bad Ending, JP |
| `Bad_EndingUS.mp4` | Bad Ending, US |


Note: The default mp4 files in the apk will be found in the /res/ directory within the apk, you shouldn't need to rename these since the converter already detects the mobile naming conventions. Discard the pencil test and 2 SEGA Forever videos, those are unneeded. If you source your mp4 files from elsewhere (ie, if you want to re implement the JP vocal tracks since the mobile version only has instrumentals for those), you must follow the naming conventions above, and the mp4 file you being along must be in 4:3 aspect ratio.

After launching terminal and booting into your KallistiOS environ run the following from the root of the repo:

If targeting CDI:
`mkdir build-dc && cd build-dc
kos-cmake -DPLATFORM=KallistiOS -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)`

If targeting /pc/ for DCload:
`mkdir build-pc && cd build-pc
kos-cmake -DPLATFORM=KallistiOS -DCMAKE_BUILD_TYPE=Release -DDC_DATA_ROOT=/pc/ ..
make -j$(nproc)`

This will generate your RSDKv3 elf

Next open the /dreamcast dir at the root of the repo and in terminal run the following:

`./generate_assets.sh [path to Data.rsdk] `
This will generate a Data folder within the dir containing converted audio assets.

Then still within the /dreamcast dir run the following:
`./dreamcast/convert_videos.sh `
This will generate an out folder within the dir containing converted FMVs. Rename this to videos

Keep note of the settings.ini in the /dreamcast dir - this will be required.

Finally run mkdcdisc in terminal with the following command to generate your CDI

`./builddir/mkdcdisc -e [Path to RSDKv3.elf] -d [path to Data dir] -d  [path to videos dir] -f [path to Data.rsdk] -f [path to settings.ini] -N -n SonicCD -o SonicCD.cdi`

if using DCload the folder structure should be as the following:

ROOT  
├── Data/  
├── videos/   
├── Data.rsdk  
├── RSDKv3.elf  
└── settings.ini
