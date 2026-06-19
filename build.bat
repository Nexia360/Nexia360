@echo off
call xb.bat build --config release
copy .\build\bin\Windows\Release\Nexia360.exe f:\NexiaTest\. /Y