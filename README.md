# TurboBuildUE4
accelerate build ue4 source code with cpp and shaders using FastBuild UE4.24 Test success


## Thoughts ##
* https://gist.github.com/hillin/b6b491ee03f22b960a7e3e90e1c6f4ec
* UE4.26 Extras/Experimental/FASTBuild/...
* https://zhuanlan.zhihu.com/p/307581961

## Install Requirements ##
* nothing

## Enable FASTBuild ##
* copy the UE4.24 some  files (same file name , maybe you should use compare tools merge them) directory to UE4.24 source code
    * /Engine/Source/Programs/UnrealBuildTool
    * /Engine/Extras
    * /Engine/Source/Programs/Automation..Utils 
    * /Engine/Source/Runtime/Engine/Public|Private
* copy the configuration.xml to  <your documents>/Unreal Engine/UnrealBuildTool/ (you can modify it for changing some options)
* Set System Environment variable , FASTBUILD_BROKERAGE_PATH  with YOUR OWN SAMBA PATH
* Start FBuildWorker.exe  on worker machines  (also need CONFIG FASTBUILD_BROKERAGE_PATH)
* Start UE4 project compiling it !

## Reference ##
* https://www.fastbuild.org/docs/download.html
* https://github.com/hillin/Unreal_FASTBuild
 
 
