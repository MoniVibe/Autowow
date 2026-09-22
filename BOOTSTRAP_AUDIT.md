# AutoWoW bootstrap audit

Generated: 2026-07-12 15:56:52 +03:00

## Source snapshot

- Core path: D:\games\wowstuff\AutoWoW\azerothcore-wotlk
- Core branch: Playerbot
- Core SHA: 52f58186a53399e603c46c24977fe60fcaad7f9d
- Module path: D:\games\wowstuff\AutoWoW\azerothcore-wotlk\modules\mod-playerbots
- Module branch: master
- Module SHA: 93aaea3de19243c09ce9ecb25627dc9671715eed
- Core dirty status: clean
- Module dirty status: clean

## Toolchain

- Git: git version 2.47.0.windows.2
- CMake: cmake version 4.0.3
- Visual Studio: Visual Studio Community 2022 17.14.36221.1 at C:\Program Files\Microsoft Visual Studio\2022\Community; complete=True | Visual Studio Build Tools 2022 17.14.36327.8 at C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools; complete=False | Visual Studio Build Tools 2019 16.11.36128.20 at C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools; complete=True
- x64 MSVC compiler: C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe
- MySQL root: D:\games\wowstuff\AutoWoW\third_party\mysql
- MySQL CLI: D:\games\wowstuff\AutoWoW\third_party\mysql\bin\mysql.exe
- MySQL service(s): NONE FOUND
- OpenSSL root: D:\games\wowstuff\AutoWoW\third_party\openssl
- OpenSSL CLI: C:\Strawberry\c\bin\openssl.exe
- Boost root: D:\games\wowstuff\AutoWoW\third_party\boost\1.83.0

## Environment variables (presence only)

| Name | Present |
|---|---|
| WOW_CLIENT_DIR | False |
| MYSQL_ROOT_DIR | False |
| MYSQL_ROOT_PASSWORD | False |
| Boost_ROOT | False |
| BOOST_ROOT | False |
| OPENSSL_ROOT_DIR | False |

## WoW client

- Client path: C:\Users\shonh\Downloads\World of Warcraft 3.3.5a
- Detection: FOUND
- Version evidence: FileVersion=; ProductVersion=; BinaryMarker12340=True
- Build 12340: **YES**

## Disk space

| Drive | Free GiB | Used GiB |
|---|---:|---:|
| C: | 376.6 | 1485.5 |
| D: | 151.3 | 779.2 |
| E: | 64.8 | 401 |
| Temp: | 376.6 | 1485.5 |

## Blocking or privileged actions

- Database initialization requires `MYSQL_ROOT_PASSWORD` in the process environment; the value is never written to this report.
- Installing a Windows service or machine-wide dependency may require administrator privileges; the current audit did not install or start anything.
- Client build 12340 has been detected; client-data extraction can proceed or has been completed by `extract-client-data.ps1`.

Audit log: D:\Games\wowstuff\AutoWoW\logs\bootstrap-20260712-155651.log
