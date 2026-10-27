@echo off
rem На CD с ответным файлом; запускается из FirstLogonCommands. Сеть (и \\10.0.2.4\qemu) при первом входе бывает не сразу:
rem ждёт общую папку до 10 минут, потом запускает bootstrap.cmd из неё (у каждой ВМ своя папка shared-<роль>).
for /l %%i in (1,1,120) do (
  if exist \\10.0.2.4\qemu\bootstrap.cmd goto run
  ping -n 6 127.0.0.1 >nul
)
:run
copy /y \\10.0.2.4\qemu\bootstrap.cmd C:\vm2bboot.cmd
call C:\vm2bboot.cmd
