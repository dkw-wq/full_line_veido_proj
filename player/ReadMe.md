
```

PS C:\Users\dkw\.a_dkwrtc\full_line_veido_proj> cmake -S player -B player/build `
   -G "Visual Studio 17 2022" -A x64 `
   -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake `
   -DVCPKG_TARGET_TRIPLET=x64-windows

.\srt_player.exe "srt://10.215.182.17:9001?mode=caller&latency=120"

.\player\build\Release\srt_player.exe "srt://10.215.182.17:9001?mode=caller&latency=120"

//2026.4.7
.\player\build\Release\srt_player.exe "srt://10.160.196.17:9001?mode=caller&latency=40&streamid=cam1" 

& "D:vcpkg\installed\x64-windows\tools\Qt6\bin\windeployqt.exe" --release player\build\Release\srt_player.exe


cmake --build player/build --config Release

```