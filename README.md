# Dark mode for CodeBlocks in Windows 10/11

This is an experimental dark mode build of CodeBlocks IDE. It uses a still-in-development version of wxWidgets 3.3, which does have a MSWEnableDarkMode() function (https://docs.wxwidgets.org/latest/classwx_app.html#af8c93d7e3345e62a58325f3ab1d158d6)

**How to run it**

Just download pre-built files from Releases, run with **CbLauncher.exe** (that way it loads configuration file with dark mode settings from a local directory - it works in portable mode)


**How to build it**
* Download master version of wxWidgets with third party libraries (--recurse-submodules) https://github.com/wxWidgets/wxWidgets
* Download SVN source of CodeBlocks https://www.codeblocks.org/downloads/svn/
* add files from this repository to CodeBlocks source code, if you are using a different code revision don't copy app.cpp, instead add MSWEnableDarkMode(DarkMode_Always); after CodeBlocksApp::OnInit()
* Download Boost (required for NassiShneiderman plugin, no need to build) https://boostorg.jfrog.io/artifactory/main/release/1.83.0/source/boost_1_83_0.zip
* Download MinGW-W64 compiler https://github.com/brechtsanders/winlibs_mingw/releases/download/13.1.0-16.0.5-11.0.0-ucrt-r5/winlibs-x86_64-posix-seh-gcc-13.1.0-mingw-w64ucrt-11.0.0-r5.7z
* Install zip (needed for C::B post-build steps) https://sourceforge.net/projects/gnuwin32/files/zip/3.0/zip-3.0-setup.exe/download  
* Copy or adjust setup.h for wxWidgets (you must #define wxUSE_GRAPHICS_DIRECT2D wxUSE_GRAPHICS_CONTEXT)
* Build wxWidgets with build_wxwidgets.bat (it looks for MinGW-W64 in .\mingw64 directory)
* Open a standard version of CodeBlocks and open CodeBlocks_wx33_64.workspace file
* Set environmental variables for wxWidgets and Boost, set path to MinGW-W64 compiler
* Right click on CodeBlocks Workspace wx3.3.x (64 bit) and click Build workspace
* Run post-build steps with update33_64.bat (add zip and bin directory of MinGW-W64 to PATH before that)
* Resulting files will be created in .\src\output33_64
* Add wxWidgets dll files to built files, add MinGW-W64 dll files http://sourceforge.net/projects/codeblocks/files/Binaries/Nightlies/Prerequisites/Mingw64dlls13.1.0.7z
* create .\AppData\codeblocks directory in CodeBlocks output directory
* copy default.conf to .\AppData\codeblocks\default.conf
* copy contents of .\src\output33_64 to wherever you want your installation of CodeBlocks be placed    
* run CodeBlocks with CbLauncher.exe (this way it will load dark mode settings from .\AppData\codeblocks\default.conf)

**Notes/issues/todo**
* Dark mode will be always on in this build, a toggle setting could be added
* Lexers XML files contain only one variant of color settings, if dark mode were to be switched on/off then a way of switching between different color settings would need to be added
* TODO: How to get rid of default.conf file and set all necessary colors directly in source code
* There are various places where colors are set, e.g. caret color is set independently of lexer colors
* TODO: Add detection of dark mode and automatic settings of appropriate colors
