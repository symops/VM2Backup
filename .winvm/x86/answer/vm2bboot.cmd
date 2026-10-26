@echo off
rem On the answer CD; run by FirstLogonCommands. The network (and so \\10.0.2.4\qemu) is not always up at the first logon:
rem waits up to 10 minutes for the share, then runs its bootstrap.cmd from a local copy.
for /l %%i in (1,1,120) do (
  if exist \\10.0.2.4\qemu\bootstrap.cmd goto run
  ping -n 6 127.0.0.1 >nul
)
:run
copy /y \\10.0.2.4\qemu\bootstrap.cmd C:\vm2bboot.cmd
call C:\vm2bboot.cmd
