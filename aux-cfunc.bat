rem invoke as
rem    .\aux-cfunc.bat analog

set sub=%1

echo Script in %sub% > log.txt

set CMPP_IDIR=../../src/xspice/icm/%sub%
set CMPP_ODIR=icm/%sub%
if not exist icm\%sub% mkdir icm\%sub%
.\bin\cmpp -lst

for /F %%n in (..\..\src\xspice\icm\%sub%\modpath.lst) do (
  echo Starting %%n >> log.txt
  set CMPP_IDIR=../../src/xspice/icm/%sub%/%%n
  set CMPP_ODIR=icm/%sub%/%%n
  if not exist icm\%sub%\%%n mkdir icm\%sub%\%%n
  .\bin\cmpp -ifs  >> log.txt
  echo Done ifs >> log.txt
  .\bin\cmpp -mod
  echo Done mod >> log.txt
  dir icm\%sub%\%%n >> log.txt
  pushd icm\%sub%\%%n
  echo In subdir icm\%sub%\%%n >> log.txt
  if exist %%n-cfunc.c del %%n-cfunc.c
  if exist %%n-ifspec.c del %%n-ifspec.c
  rename cfunc.c %%n-cfunc.c
  rename ifspec.c %%n-ifspec.c
  popd
)

