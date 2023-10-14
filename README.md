# D'oh!

This is a port of The Simpsons Hit & Run to the Switch based on the leaked source code. The full game should be playable, including local multiplayer in the bonus game. The OpenGL renderer is however still incomplete, so some visual glitches can be observed and some special effects are missing compared to the PC version.

Please report any bugs or feature requests in the issues tab on this Github repository.

# Installation

This port uses the PC assets, so you will need to have the PC version of the game installed. Do not use the assets from the source code leak as those are not the final version, instead use the assets from the official release.

Make a copy of your installation folder from the official PC release and then extract [the release zip file](https://github.com/ZenoArrows/The-Simpsons-Hit-and-Run/releases) into that folder replacing any existing files.

To add support for cutscenes you will need to acquire a copy of a [Bink 2 encoder](https://github.com/marcussacana/Bink2), place the executable in the movies folder and then run convert.bat to convert the Bink 1 cutscenes to Bink 2.

Finally copy the folder to the switch folder on your sd card and launch the game from the hbmenu.

# Multi-Language support

The PAL version supports multiple languages and will use the language that matches the system language of your Switch. If your Switch is set to a language that is not supported a menu will be shown giving you the option to choose between the supported languages.

No official release has the dialog RCF files for all 4 supported languages, so you will need to make sure you use the game assets from a release that's localized in the language you'd like to play.

If you'd just like to play in English and have no need for multi-language support, then use the NTSC version to play.
