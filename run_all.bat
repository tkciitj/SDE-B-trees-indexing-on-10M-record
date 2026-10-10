@echo off
rem Usage: run_all.bat path\to\dataset.csv   (needs g++ from MinGW-w64 / MSYS2)
if "%~1"=="" (echo usage: run_all.bat dataset.csv & exit /b 1)
g++ -O2 -std=c++17 -Wall -Wextra -o bpt.exe src/main.cpp -lpsapi || exit /b 1
bpt.exe inspect %1
bpt.exe test
del /q results.csv 2>nul
bpt.exe mem  %1
bpt.exe disk %1
for %%P in (1024 4096 16384 65536) do bpt.exe buf %1 --cache-pages %%P
echo All runs appended to results.csv
