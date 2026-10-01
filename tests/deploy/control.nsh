@echo -off

rem Control run: chainload the boot manager without loading undefshim first,
rem to tell a slow firmware load apart from something the driver caused.

for %d in fs0 fs1 fs2 fs3
  if exist %d:\EFI\Microsoft\Boot\bootmgfw.efi then
    echo CONTROL: chainloading bootmgfw on %d
    %d:\EFI\Microsoft\Boot\bootmgfw.efi
    echo CONTROL: bootmgfw returned
  endif
endfor

echo CONTROL: done
