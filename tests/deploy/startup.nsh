@echo -off

for %d in fs0 fs1 fs2 fs3
  if exist %d:\undefshim_driver.efi then
    echo UNDEFSHIM: driver on %d
    load -nc %d:\undefshim_driver.efi
  endif
endfor

for %d in fs0 fs1 fs2 fs3
  if exist %d:\EFI\Microsoft\Boot\bootmgfw.efi then
    echo UNDEFSHIM: chainloading bootmgfw on %d
    %d:\EFI\Microsoft\Boot\bootmgfw.efi
    echo UNDEFSHIM: bootmgfw returned
  endif
endfor

echo UNDEFSHIM: startup.nsh done
