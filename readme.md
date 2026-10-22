特别鸣谢：[AlexandreRouma](https://github.com/AlexandreRouma/SDRPlusPlus)源代码，[nkxingxh](https://github.com/nkxingxh/SDRPlusPlus-CHS)汉化源码，[sannysanoff](https://github.com/sannysanoff/SDRPlusPlusBrown) kiwisdr集成

# SDR++, 轻量的 SDR 软件<br>

SDR++ 是一款跨平台且开源的软件定义无线电 (SDR) 软件，其设计目标是精简高效且易于使用。

> 当前分支是 SDR++ 的简体中文版本。
> 当前版本v1.3.0

<img width="1920" height="1020" alt="main" src="https://github.com/user-attachments/assets/7f2603d0-2cec-44c2-8be3-debd760ec799" />
<img width="1920" height="1020" alt="map" src="https://github.com/user-attachments/assets/b32c6252-be67-467b-8e51-e68ce048569c" />

## 特性

* 多频点调谐器 (Multi VFO) 
* 广泛的硬件支持 (both through SoapySDR and dedicated modules) 
* 支持 SIMD 加速的数字信号处理 (DSP) 
* 跨平台支持 (Windows、Linux、MacOS、BSD 和 Android) 
* 在可能的情况下实现瀑布图全图更新，使信号浏览更加轻松和愉悦
* 模块化设计 (可轻松编写自己的插件)
* 支持kiwisdr信号源

# 下载

## 简体中文版本

[发布页面](https://github.com/blueveryday/SDRPlusPlus_Chinese/releases)下载最新版本


## 英文原版

[发布页面](https://github.com/AlexandreRouma/SDRPlusPlus/releases)下载最新版本

# 模块列表

Not all modules are built by default. I decided to disable the build of those with large libraries, libraries that can't be installed through the package manager (or pothos) and those that are still in beta.
Modules in beta are still included in releases for the most part but not enabled in SDR++ (need to be instantiated).

## Sources

| Name                 | Stage      | Dependencies      | Option                         | Built by default| Built in Release        | Enabled in SDR++ by default |
|----------------------|------------|-------------------|--------------------------------|:---------------:|:-----------------------:|:---------------------------:|
| airspy_source        | Working    | libairspy         | OPT_BUILD_AIRSPY_SOURCE        | ✅              | ✅                     | ✅                         |
| airspyhf_source      | Working    | libairspyhf       | OPT_BUILD_AIRSPYHF_SOURCE      | ✅              | ✅                     | ✅                         |
| audio_source         | Working    | rtaudio           | OPT_BUILD_AUDIO_SOURCE         | ✅              | ✅                     | ✅                         |
| bladerf_source       | Working    | libbladeRF        | OPT_BUILD_BLADERF_SOURCE       | ⛔              | ✅ (not Debian Buster) | ✅                         |
| file_source          | Working    | -                 | OPT_BUILD_FILE_SOURCE          | ✅              | ✅                     | ✅                         |
| fobossdr_source      | Working    | libfobos          | OPT_BUILD_FOBOSSDR_SOURCE      | ✅              | ✅                     | ✅                         |
| hackrf_source        | Working    | libhackrf         | OPT_BUILD_HACKRF_SOURCE        | ✅              | ✅                     | ✅                         |
| harogic_source       | Beta       | htra_api          | OPT_BUILD_HAROGIC_SOURCE       | ⛔              | ⛔                     | ✅                         |
| hermes_source        | Beta       | -                 | OPT_BUILD_HERMES_SOURCE        | ✅              | ✅                     | ✅                         |
| kcsdr_source         | Unfinished | libkcsdr          | OPT_BUILD_KCSDR_SOURCE         | ⛔              | ⛔                     | ⛔                         |
| limesdr_source       | Working    | liblimesuite      | OPT_BUILD_LIMESDR_SOURCE       | ⛔              | ✅                     | ✅                         |
| network_source       | Beta       | -                 | OPT_BUILD_NETWORK_SOURCE       | ✅              | ✅                     | ✅                         |
| perseus_source       | Beta       | libperseus-sdr    | OPT_BUILD_PERSEUS_SOURCE       | ⛔              | ✅                     | ✅                         |
| plutosdr_source      | Working    | libiio, libad9361 | OPT_BUILD_PLUTOSDR_SOURCE      | ✅              | ✅                     | ✅                         |
| rfnm_source          | Beta       | librfnm           | OPT_BUILD_RFNM_SOURCE          | ⛔              | ✅                     | ✅                         |
| rfspace_source       | Working    | -                 | OPT_BUILD_RFSPACE_SOURCE       | ✅              | ✅                     | ✅                         |
| rtl_sdr_source       | Working    | librtlsdr         | OPT_BUILD_RTL_SDR_SOURCE       | ✅              | ✅                     | ✅                         |
| rtl_tcp_source       | Working    | -                 | OPT_BUILD_RTL_TCP_SOURCE       | ✅              | ✅                     | ✅                         |
| sdrplay_source       | Working    | SDRplay API       | OPT_BUILD_SDRPLAY_SOURCE       | ⛔              | ✅                     | ✅                         |
| sdrpp_server_source  | Working    | -                 | OPT_BUILD_SDRPP_SERVER_SOURCE  | ✅              | ✅                     | ✅                         |
| soapy_source         | Deprecated | soapysdr          | OPT_BUILD_SOAPY_SOURCE         | ⛔              | ⛔                     | ⛔                         |
| spectran_source      | Unfinished | RTSA Suite        | OPT_BUILD_SPECTRAN_SOURCE      | ⛔              | ⛔                     | ⛔                         |
| spectran_http_source | Beta       | -                 | OPT_BUILD_SPECTRAN_HTTP_SOURCE | ✅              | ✅                     | ✅                         |
| spyserver_source     | Working    | -                 | OPT_BUILD_SPYSERVER_SOURCE     | ✅              | ✅                     | ✅                         |
| usrp_source          | Beta       | libuhd            | OPT_BUILD_USRP_SOURCE          | ⛔              | ⛔                     | ✅                         |
| kiwisdr              | Beta       | -                 | OPT_BUILD_KIWISDR_SOURCE       | ✅              | ✅                     | ✅                         |

## Sinks

| Name               | Stage      | Dependencies | Option                       | Built by default| Built in Release | Enabled in SDR++ by default |
|--------------------|------------|--------------|------------------------------|:---------------:|:----------------:|:---------------------------:|
| android_audio_sink | Working    | -            | OPT_BUILD_ANDROID_AUDIO_SINK | ⛔              | ✅              | ✅ (Android only)          |
| audio_sink         | Working    | rtaudio      | OPT_BUILD_AUDIO_SINK         | ✅              | ✅              | ✅                         |
| network_sink       | Working    | -            | OPT_BUILD_NETWORK_SINK       | ✅              | ✅              | ✅                         |
| new_portaudio_sink | Working    | portaudio    | OPT_BUILD_NEW_PORTAUDIO_SINK | ⛔              | ✅              | ⛔                         |
| portaudio_sink     | Working    | portaudio    | OPT_BUILD_PORTAUDIO_SINK     | ⛔              | ✅              | ⛔                         |

## Decoders

| Name                | Stage      | Dependencies | Option                        | Built by default| Built in Release | Enabled in SDR++ by default |
|---------------------|------------|--------------|-------------------------------|:---------------:|:----------------:|:---------------------------:|
| atv_decoder         | Unfinished | -            | OPT_BUILD_ATV_DECODER         | ⛔              | ⛔              | ⛔                         |
| dab_decoder         | Unfinished | -            | OPT_BUILD_DAB_DECODER         | ⛔              | ⛔              | ⛔                         |
| falcon9_decoder     | Unfinished | ffplay       | OPT_BUILD_FALCON9_DECODER     | ⛔              | ⛔              | ⛔                         |
| kgsstv_decoder      | Unfinished | -            | OPT_BUILD_KGSSTV_DECODER      | ⛔              | ⛔              | ⛔                         |
| m17_decoder         | Working    | -            | OPT_BUILD_M17_DECODER         | ⛔              | ✅              | ⛔                         |
| meteor_demodulator  | Working    | -            | OPT_BUILD_METEOR_DEMODULATOR  | ✅              | ✅              | ⛔                         |
| pager_decoder       | Unfinished | -            | OPT_BUILD_PAGER_DECODER       | ⛔              | ⛔              | ⛔                         |
| radio               | Working    | -            | OPT_BUILD_RADIO               | ✅              | ✅              | ✅                         |
| radio               | Unfinished | -            | OPT_BUILD_VOR_RECEIVER        | ⛔              | ⛔              | ⛔                         |
| weather_sat_decoder | Unfinished | -            | OPT_BUILD_WEATHER_SAT_DECODER | ⛔              | ⛔              | ⛔                         |

## Misc

| Name                | Stage      | Dependencies | Option                      | Built by default | Built in Release | Enabled in SDR++ by default |
|---------------------|------------|--------------|-----------------------------|:----------------:|:----------------:|:---------------------------:|
| discord_integration | Working    | -            | OPT_BUILD_DISCORD_PRESENCE  | ✅              | ✅               | ⛔                         |
| frequency_manager   | Working    | -            | OPT_BUILD_FREQUENCY_MANAGER | ✅              | ✅               | ✅                         |
| iq_exporter         | Working    | -            | OPT_BUILD_IQ_EXPORTER       | ✅              | ✅               | ⛔                         |
| recorder            | Working    | -            | OPT_BUILD_RECORDER          | ✅              | ✅               | ✅                         |
| rigctl_client       | Unfinished | -            | OPT_BUILD_RIGCTL_CLIENT     | ✅              | ✅               | ⛔                         |
| rigctl_server       | Working    | -            | OPT_BUILD_RIGCTL_SERVER     | ✅              | ✅               | ✅                         |
| scanner             | Beta       | -            | OPT_BUILD_SCANNER           | ✅              | ✅               | ⛔                         |
| scheduler           | Unfinished | -            | OPT_BUILD_SCHEDULER         | ⛔              | ⛔               | ⛔                         |

# Troubleshooting

First, please make sure you're running the latest automated build. If your issue is linked to a bug it is likely that is has already been fixed in later releases

## SDR++ crashes then it won't start again no matter what

This is a bug in 1.0.0 that was fixed in 1.0.1

In some cases, if a crash happened while the config was being saved, the config file would be corrupted and SDR++ would refuse to start because of it.

This has now been fixed. If a config file is corrupted it'll just reset it to its default state.

## "hash collision" error when starting

You likely installed the `soapysdr-module-all` package on Ubuntu/Debian. If not it's still a SoapySDR bug caused by multiple soapy modules coming in conflict. Uninstall anything related to SoapySDR then install soapysdr itself and only the soapy modules you actually need.

## "I don't see -insert module name here-, what's going on?"

If the module was included in a later update, it's not enabled in the config. The easiest way to fix this is just to delete the `config.json` file and let SDR++ recreate it (you will lose your setting relating to the main UI like VFO colors, zoom level and theme).
The best option however is to edit the config file to add an instance of the module you wish to have enabled (see the Module List).

## SDR++ crashes when stopping a RTL-SDR

This is a bug recently introduced by libusb1.4
To solve, this, simply downgrade to libusb1.3

## SDR++ crashes when starting a HackRF

If you also have the SoapySDR module enabled, this is a bug in libhackrf. It's caused by libhackrf not checking if it's already initialized.
The solution until a fixed libhackrf version is released is to disable the soapy_source module from SDR++. For this, go into the "模块管理器" menu and click the `-` button next to the row with "soapy_source". After that, restart SDR++.

## Issue not listed here?

If you still have an issue, please open an issue about it or ask on the discord. I'll try to respond as quickly as I can. Please avoid trying to contact me on every platform imaginable thinking I'll respond faster though...

# Contributing

Feel free to submit band plans via the GitHub issue tracker.
For code changes, please create a feature request instead.

# Credits

## Patrons

* Bob Logan
* [Christian Häusler](https://github.com/corvus-ch)
* Croccydile
* Dale L Puckett (K0HYD)
* [Daniele D'Agnelli](https://linkedin.com/in/dagnelli)
* [David Taylor (GM8ARV)](https://twitter.com/gm8arv)
* D. Jones
* Dexruus
* [EB3FRN](https://www.eb3frn.net/)
* Eric Johnson
* Ernest Murphy (NH7L)
* Flinger Films
* [Frank Werner (HB9FXQ)](https://twitter.com/HB9FXQ)
* gringogrigio
* Jandro
* Jeff Moe
* Joe Cupano
* KD1SQ
* Kezza
* Krys Kamieniecki
* Lee Donaghy
* Lee (KD1SQ)
* .lozenge. (Hank Hill)
* Martin Herren (HB9FXX)
* NeoVilsonWong
* Nitin (VU2JEK)
* ON4MU
* [Passion-Radio.com](https://passion-radio.com/)
* Paul Maine
* Peter Betz
* [Scanner School](https://scannerschool.com/)
* Scott Palmer
* [SignalsEverywhere](https://signalseverywhere.com/)
* Syne Ardwin (WI9SYN)
* [W4IPA](https://twitter.com/W4IPAstroke5)
* William Arcand (W1WRA)
* William Pitchford
* [Yves Rougy](https://www.twitch.tv/yorzian)
* [Zipper](https://github.com/reppiZ)

## Contributors

* [Aang23](https://github.com/Aang23)
* [Alexsey Shestacov](https://github.com/wingrime)
* [Aosync](https://github.com/aosync)
* [Benjamin Kyd](https://github.com/benkyd)
* [Benjamin Vernoux](https://github.com/bvernoux)
* [Cropinghigh](https://github.com/cropinghigh)
* [Fred F4EED](http://f4eed.wordpress.com/)
* [Howard0su](https://github.com/howard0su)
* John Donkersley
* [Joshua Kimsey](https://github.com/JoshuaKimsey)
* [Manawyrm](https://github.com/Manawyrm)
* [Martin Hauke](https://github.com/mnhauke)
* [Marvin Sinister](https://github.com/marvin-sinister)
* [Maxime Biette](https://github.com/mbiette)
* [Paulo Matias](https://github.com/thotypous)
* [Raov](https://twitter.com/raov_birbtog)
* [Cam K.](https://github.com/Starman0620)
* [Shuyuan Liu](https://github.com/shuyuan-liu)
* [Syne Ardwin (WI9SYN)](https://esaille.me/)
* [Szymon Zakrent](https://github.com/zakrent)
* Youssef Touil
* [Zimm](https://github.com/invader-zimm)


## Libraries used

* [SoapySDR (PothosWare)](https://github.com/pothosware/SoapySDR)
* [Dear ImGui (ocornut)](https://github.com/ocornut/imgui)
* [json (nlohmann)](https://github.com/nlohmann/json)
* [rtaudio](http://www.portaudio.com/)
* [Portable File Dialogs](https://github.com/samhocevar/portable-file-dialogs)
