@echo -off

rem Exercises the LoadImage hook with an image that returns.
rem
rem The first load installs the hook. The second goes through it, and unlike
rem the boot manager this image comes back, so the whole path can be observed:
rem the hook fires, the original returns, and the driver runs a second time.

for %d in fs0 fs1 fs2 fs3
  if exist %d:\undefshim_driver.efi then
    echo HOOKTEST: first load on %d
    load -nc %d:\undefshim_driver.efi
    echo HOOKTEST: second load on %d
    load -nc %d:\undefshim_driver.efi
    echo HOOKTEST: second load returned
  endif
endfor

echo HOOKTEST: done
