@echo -off

for %d in fs0 fs1 fs2 fs3
  if exist %d:\undefshim_driver.efi then
    echo UNDEFSHIM: driver on %d
    load -nc %d:\undefshim_driver.efi
  endif
endfor

for %d in fs0 fs1 fs2 fs3
  if exist %d:\ntoskrnl.efi then
    echo UNDEFSHIM: running the fake kernel on %d
    %d:\ntoskrnl.efi
  endif
endfor

echo FAKEK: returned
