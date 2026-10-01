# Guitar Pedalboard：電吉他綜合效果器 (VST3 + 獨立程式)

用 C++ / JUCE 9 寫的電吉他效果器。介面仿 Line 6 POD HD500X 的訊號鏈：**8 個效果格 + 1 個 AMP / CAB 方塊**（50 種音箱頭、22 種音箱、14 種麥克風），最後接一組 6 段 EQ 和雜訊處理，上方有可開關的調音器：

```
吉他 → [HUM 濾除] → [1] [2] [AMP/CAB] [3] [4] [5] [6] [7] [8] → EQ → DENOISE → 輸出
        每一格可以放任何效果、任意排順序（預設：Noise Gate > Screamer > 412 Classic 音箱 > Chorus > Analog Delay > Room Reverb）
```

- **點一個方塊**：下方的編輯區顯示它的旋鈕，可以換「效果類型」和「型號」。
- **雙擊方塊** 或踩編輯區右邊的腳踏開關：開 / 關（30 ms 淡入淡出，不會爆音）。
- **拖曳方塊**（或按 `< MOVE` / `MOVE >`）換位置，例如把 AMP 方塊移到破音前面。
- 空的格子選一個效果類型就會放進去；`CLEAR` 清空。換型號時舊的會先淡出、新的再淡入。
- DELAY / REVERB 關掉後尾音會自然消失 (trails)；破音以 4 倍超取樣處理，減少數位雜音。
- 旋鈕：上下拖曳調整，**雙擊回到預設值**，也可以直接點數字輸入。

### 效果型號（8 個效果格可放的 118 種 + AMP / CAB 方塊）

| 類別 | 型號 |
|---|---|
| Dynamics (9) | Noise Gate、Hard Gate、Tube Comp、Red Comp、Blue Comp、Blue Comp Treb、Vetta Comp、Vetta Juice、Boost Comp |
| Distortion (15) | Tube Drive、Screamer、Overdrive、Classic Dist、Heavy Dist、Color Drive、Buzz Saw、Facial Fuzz、Jumbo Fuzz、Fuzz Pi、Jet Fuzz、Line 6 Drive、Line 6 Distortion、Sub Octave Fuzz、Octave Fuzz |
| Modulation (26) | Chorus、Flanger、Classic Phaser、Tremolo（本專案原有的 4 種）、Pattern Tremolo、Panner、Bias Tremolo、Opto Tremolo、Script Phase、Panned Phaser、Barberpole Phaser、Dual Phaser、U-Vibe、Phaser、Pitch Vibrato、Dimension、Analog Chorus、Tri Chorus、Analog Flanger、Jet Flanger、AC Flanger、80A Flanger、Frequency Shifter、Ring Modulator、Rotary Drum、Rotary Drm/Hrn |
| Filter / Synth (16) | Voice Box、V-Tron、Q Filter、Seeker、Obi Wah、Tron Up、Tron Down、Throbber、Slow Filter、Spin Cycle、Comet Trails、Octisynth、Synth O Matic、Attack Synth、Synth String、Growler |
| Pitch (3) | Bass Octaver、Pitch Glide、Smart Harmony |
| Preamp + EQ (6) | Graphic EQ、Parametric EQ、Studio EQ、4 Band Shift EQ、Mid Focus EQ、Vintage Pre |
| Delay (20) | Analog Delay（原有）、Ping Pong、Dynamic Dly、Stereo Delay、Digital Delay、Dig Dly W/Mod、Reverse、Lo Res Delay、Tube Echo、Tube Echo Dry、Tape Echo、Tape Echo Dry、Sweep Echo、Sweep Echo Dry、Echo Platter、Echo Platter Dry、Analog W/Mod、Analog Echo、Auto-Volume Echo、Multi-Head |
| Reverb (13) | Room Reverb（原有）、Plate、Room、Chamber、Hall、Echo、Tile、Cave、Ducking、Octo、Spring、'63 Spring、Particle Verb |
| Wah (8) | Fassel、Conductor、Throaty、Colorful、Vetta Wah、Chrome、Chrome Custom、Weeper |
| Volume / Pan (2) | Volume Pedal、Pan |
| **AMP**（50 種） | Blackface Double Nrm/Vib、Hiway 100、Super O、Gibtone 185、Tweed B-Man Nrm/Brt、Blackface 'Lux Nrm/Vib、Divide 9/15、PhD Motorway、Class A-15、Class A-30 TB、Brit J-45 Nrm/Brt、Plexi Lead 100 Nrm/Brt、Brit P-75 Nrm/Brt、Brit J-800、Bomber Uber、Treadplate、Angel F-Ball、Line 6 Elektrik、Solo 100 Clean/Crunch/OD、Line 6 Doom、Line 6 Epic、Flip Top、PV Panama、Mahadeva、Brit 2204、Line 6 Insane、Big Bottom、Variac'ed Plexi、Purge、Aggro、Smash、Octone、Jazz Rivet、Small Tweed、Mandarin 80、A30 Fawn Nrm/Brt、Black Panel Pete、Line 6 Acoustic、SVT Nrm/Brt、G Cougar 800 |
| **CAB**（22 種） | 212 Blackface Double、412 Hiway、6x9 Super O、112 Field Coil、410 Tweed、112 BF 'Lux、112 Celest 12-H、212 PhD Ported、112 Blue Bell、212 Silver Bell、412 Greenback 25、412 Blackback 30、412 Brit T-75、412 Uber、412 Tread V-30、412 XXL V-30、115 Flip Top、212 Jazz Rivet、108 Small Tweed、810 SV Beast、410 Rhino、412 Classic（本專案原本的音箱聲音） |
| 輸出端 | EQ 等化器（6 段，詳見下方）、NOISE（Hum 濾除、Denoise） |

- **旋鈕**跟 HD500X 大致相同：破音是 Drive、Bass、Mid、Treble、Output；音箱頭是 Drive、Bass、Mid、Treble、Presence、Ch Vol、Master、Sag、Hum、Bias、Bias X、Power Amp（Off = 只有前級）；音箱是 Mic（14 種麥克風）、E.R.（早期反射）、Low Cut、Res Level、Thump、Decay。
- **選音箱頭時會自動換成它通常搭配的音箱**（跟 HD500X 一樣），之後可以自己換。「No Amp」+ 412 Classic 就是 1.1 版的聲音。
- **延遲的 Note** 都可以跟 TAP 同步；Stereo Delay 左右各有一組。Wah 的 Position、Volume Pedal 的 Position 是一般旋鈕（沒有踏板），可以用 DAW 的自動化控制。
- DELAY / REVERB 關掉後尾音會自然消失 (trails)。Tube Echo、Tape Echo、Sweep Echo、Echo Platter 連原音也會經過機器前級的染色；它們的「Dry」版本重複音一樣，但原音保持乾淨。

**型號名稱照 HD500X 的清單，方便對照；聲音是依照每台原型機器的電路和一般已知特性自己寫的近似模型，不是 Line 6 的原始演算法，也沒有對照實機量測**，和 Line 6 沒有關係。每個型號在預設值時音量大致相同，換型號不會突然變大聲。

**沒有做的部分**：Vocoder（需要硬體的麥克風輸入）、FX Loop、Looper、雙音箱頭並聯（HD500X 的 Path A/B）、輸入阻抗 (Input Z) 設定、表情踏板（Position 類旋鈕可用 DAW 自動化代替）。

### 雜訊處理（EQ 區左下角的 NOISE）

| 功能 | 位置 | 作用 |
|---|---|---|
| **Hum**：Off / 50 Hz / 60 Hz | 吉他輸入端（所有效果之前） | 濾掉電源哼聲和它的泛音（台灣、美洲用 60 Hz；歐洲、中國用 50 Hz），在破音把它放大之前就處理掉。實測哼聲降低 100 dB 以上，一般音符幾乎不受影響（最靠近泛音的音約少 0.5–1.3 dB） |
| **Denoise**：0–100 % | 最後輸出端 | 彈奏時完全打開，聲音消失時自動把嘶聲壓下去（實測約降 24–27 dB）；50 % 以下不影響彈奏的音量，開到 100 % 時樂句整體約少 1.2 dB、音尾稍微變暗 |

兩者都不會增加延遲。**調音器**：點一下就能開 / 關（關掉後不做音高偵測、省 CPU；調音器本來就不會增加延遲）。

## TAP 拍速

訊號鏈右邊的 **TAP** 是全域速度（跟 HD500X 的 TAP 鍵一樣）：

1. 跟著音樂的拍子 (四分音符) 在 **TAP** 上點 3～4 下 — 按下去的瞬間就算一拍；也可以直接輸入 BPM。
2. 每個 DELAY 型號的 **Note** 選音符值（預設 1/4）時，延遲時間會自動等於「一拍 × 音符長度」，TIME 旋鈕變灰並顯示換算後的時間；選 `ms` 就回到用旋鈕設定。Stereo Delay 左右兩邊各有一組 Time / Note。
3. Room Reverb 的 **Note** 同理，控制 Pre-Delay（殘響開始前的空檔）。

| 效果 | 音符長度選項 | 例：120 BPM 時 |
|---|---|---|
| 所有 Delay 型號 | 1/4、1/8.(附點八分)、1/8、1/8T(八分三連音)、1/16 | 1/4 = 500 ms、1/8. = 375 ms (U2 The Edge 的經典設定) |
| Room Reverb | 1/32、1/16、1/8、1/4 | 1/16 = 125 ms 的 pre-delay |

- 點得不準也沒關係，會取最近幾下的平均；停超過 2 秒再點會重新開始計算。

## 沒有接吉他時測試

上方的 **INPUT** 列可以選擇聲音來源：

| 按鈕 | 作用 |
|---|---|
| **LIVE INPUT** | 平常用法：音訊介面上的吉他 |
| **DEMO RIFF** | 播放內建的吉他樂句 (約 11 秒循環)：悶音刷弦、強力和弦、五聲音階旋律、最後一個延音和弦 (方便聽延遲 / 殘響的尾音) |
| **AUDIO FILE** | 循環播放你載入的音檔 (**LOAD FILE...**，支援 WAV / MP3 / FLAC / AIFF / OGG)。建議用吉他 **DI 乾聲** (沒有效果、直接錄進介面的聲音)，網路上搜尋「guitar DI track」可以找到很多 |
| **STOP / PLAY** | 停止 / 從頭播放 DEMO RIFF 或音檔（選 LIVE INPUT 時不會出現） |
| **PLUCK  E A D G B e** | 撥一根標準音高的空弦，可以測試調音器 (會顯示 IN TUNE)；按下時會自動停止 DEMO / 音檔，讓你單獨聽那根弦 |
| **CHORD** | 基本開放和弦：C (x32010)、D (xx0232)、E (022100)、F (133211)、G (320003)、A (x02220)、Am (x02210)、Dm (xx0231)、Em (022000)。不彈的弦 (x) 會被悶掉；會自動停止 DEMO |
| **彈法 / BPM / STOP** | **Arpeggio**：持續的分散和弦（低音、G、B、e、另一個低音、G、B、e，八分音符）；**Strum loop**：每一拍往下刷一次（四分音符，第一拍稍重）；**Single**：刷一次。BPM 可以直接輸入 40–240。循環中按別的和弦，Arpeggio 會在下一個八分音符換過去並從低音開始，Strum loop 會在下一拍換過去；STOP 停止並悶住弦 |

- 選 DEMO RIFF / AUDIO FILE 時，程式完全不使用麥克風和介面的輸入，所以**只要有耳機或喇叭就能測試**，不會回授。
- 進度條會顯示循環播放的位置；選擇會被記住，下次開啟會延續上次的設定。
- 想改回彈真吉他時，記得切回 **LIVE INPUT**。

## EQ 使用方式

EQ 在視窗最下方，圖上有兩層資訊：
- **白色曲線**：目前 EQ 的頻率響應 (哪些頻率被加強 / 減弱)；選到的頻段會用它的顏色把自己的影響範圍塗出來。
- **藍色頻譜**：聲音經過 EQ 之後的即時頻譜 (ANALYZER 按鈕可開關)。彈琴時可以看到哪些頻率最多。

| 頻段 | 類型 | 可調 |
|---|---|---|
| LOW CUT | 低頻切除 (高通) | 頻率 20–600 Hz，拉到 20 Hz 等於關閉；切掉轟轟的低頻 |
| BASS | 低頻架式 (low shelf) | 頻率 40–500 Hz、增益 ±15 dB |
| LO MID | 參數式 (peak) | 頻率 100 Hz–2 kHz、增益 ±15 dB、Q 0.3–6 |
| HI MID | 參數式 (peak) | 頻率 500 Hz–8 kHz、增益 ±15 dB、Q 0.3–6 |
| TREBLE | 高頻架式 (high shelf) | 頻率 1.5–15 kHz、增益 ±15 dB |
| HIGH CUT | 高頻切除 (低通) | 頻率 1–20 kHz，拉到 20 kHz 等於關閉；去掉刺耳的高頻 |

操作：
- **拖曳圖上的彩色圓點**：左右 = 頻率，上下 = 增益。
- **滑鼠滾輪** (在 LO MID / HI MID 圓點上)：調整 Q (頻寬，數字越大越窄)。
- **雙擊圓點**：該頻段回到預設值；**FLAT** 按鈕：全部歸零。
- 右邊的頻段按鈕 + 旋鈕可以精確調整目前選到的頻段，也可以點旋鈕下方的數字直接輸入 (例如 `2.5k`)。
- 常用起點：破音太悶 → HI MID 在 2–3 kHz 加 3 dB；太刺 → HIGH CUT 拉到 6–8 kHz；低音太糊 → LOW CUT 拉到 80–100 Hz；經典 metal scoop → LO MID 在 500 Hz 減 6 dB。

## 在別台電腦 / 手機上使用

### Windows：安裝版與免安裝版（`dist\`）

| 檔案 | 說明 |
|---|---|
| `dist\GuitarPedalboard-1.2.1-Setup.exe` | 安裝程式：開始功能表捷徑、可選桌面捷徑、可選安裝 VST3、有解除安裝 |
| `dist\GuitarPedalboard-1.2.1-Windows-Portable.zip` | 免安裝：解壓縮後直接執行 `Guitar Pedalboard.exe`；`Install-VST3.cmd` 可另外安裝外掛 |

- 程式已靜態連結 C++ 執行階段，**不需要另外安裝 Visual C++ Redistributable**，Windows 10 / 11 64 位元都能直接執行。
- 因為沒有付費的程式碼簽章憑證，第一次執行可能出現「Windows 已保護您的電腦」：按「其他資訊」→「仍要執行」。
- 重新打包：`powershell -ExecutionPolicy Bypass -File packaging\build-windows.ps1`（需要 Inno Setup 6，放在 `tools\InnoSetup6`）。
- 給別人下載：`powershell -ExecutionPolicy Bypass -File packaging\publish-release.ps1` 會把 `dist\` 裡這個版本的安裝檔和 zip 放到 GitHub 的 Releases（https://github.com/Anyouhuang/guitar-pedalboard/releases）。

### 網頁版（手機、Mac、任何電腦的瀏覽器）

網頁版的音效處理是 C++ 版逐行移植的 JavaScript（`web/src/dsp/`），在瀏覽器的 AudioWorklet 裡即時運算；`node web/test/compare.mjs` 會把同一段輸入同時跑過 C++ 與 JavaScript，確認每個效果的輸出差異都在 -70 dB 以下（聽不出差別）。8 格都放最吃 CPU 的效果再加高增益音箱頭，在一般電腦上約用掉一個 CPU 核心的 12%；較舊的手機若出現斷音，請減少同時開啟的效果。

| 方式 | 網址 / 做法 | 能接吉他嗎 |
|---|---|---|
| **GitHub Pages（可接吉他）** | **https://anyouhuang.github.io/guitar-pedalboard/** 。更新：`powershell -ExecutionPolicy Bypass -File web\deploy-github.ps1`（會重新建置並只上傳有變動的檔案；GitHub 登入資訊由 `tools\gh` 保存） | ✓ 手機接 USB-C 錄音介面、電腦接介面都可以 |
| 這台電腦本機 | `node web/serve.mjs`，瀏覽器開 http://localhost:5173 | ✓ |

- 修改網頁版：編輯 `web/src/`，執行 `node web/build.mjs` 重新產生 `web/dist/`，再用 `web\deploy-github.ps1` 發佈。
- 麥克風權限：第一次選 LIVE INPUT 時瀏覽器會詢問，請按「允許」。如果按過「不允許」：iPhone Safari 點網址列左邊的「大小」圖示 →「網站設定」→「麥克風」→「允許」；電腦版 Chrome / Edge 點網址列左邊的圖示 →「麥克風」→「允許」，然後重新整理。
- 瀏覽器規定 AudioWorklet 和麥克風只能在 HTTPS 或 localhost 使用，用 `http://電腦IP` 從手機連線是無法運作的。
- 瀏覽器的延遲比 ASIO 高，一般約 10–40 ms，視裝置而定；要最低延遲請用 Windows 版 + ASIO。
- iPhone / iPad：Web Audio 會被側邊的靜音開關關掉。頁面被嵌在別的網頁裡時（例如 claude.ai），聲音會改走 `<audio>` 媒體通道（跟影片一樣，不受靜音開關影響，延遲稍高）；放在自己的網站時則直接輸出，並把音訊類型設成「播放」。頁面最下方的「音訊狀態」會顯示目前走哪一條。

## 編譯好的檔案

| 檔案 | 用途 |
|---|---|
| `build\GuitarPedalboard_artefacts\Release\Standalone\Guitar Pedalboard.exe` | 獨立程式，直接開啟就能彈 |
| `build\GuitarPedalboard_artefacts\Release\VST3\Guitar Pedalboard.vst3` | VST3 外掛，給 DAW 使用 |
| `test_output\*.wav` | 測試程式產生的各效果試聽檔 |

## 使用方式

### A. 獨立程式 (最簡單)

1. 吉他接到錄音介面 (Audio Interface) 的 Hi-Z / Instrument 輸入。
2. 開啟 `Guitar Pedalboard.exe`，按左上角 **Options → Audio/MIDI Settings**：
   - **Audio device type** 選 **ASIO**，選擇你的錄音介面驅動 (沒有 ASIO 驅動可以裝 [ASIO4ALL](https://asio4all.org/)，或改用 Windows Audio (Exclusive Mode))。
   - **Active input channels** 只勾吉他所在的那個輸入。
   - **Audio buffer size** 設 64～128 samples，延遲比較低。
3. 視窗上方黃色提示「Audio input is muted to avoid feedback loop」：這是 JUCE 預設的防回授保護，請到同一個設定畫面把 **Mute audio input** 取消勾選 (只要設定一次，之後會記住)。
   ⚠️ 請戴耳機或接音箱，不要用筆電內建麥克風 + 喇叭，否則會產生回授尖叫。
4. 用耳機或監聽喇叭聽的時候請開 **AMP / CAB** 方塊；接真的吉他音箱時請關掉（或只留音箱頭、把 CAB 選成 No Cab）。

### B. VST3 外掛 (在 Reaper、Cubase、Studio One、FL Studio 等 DAW 中使用)

以**系統管理員**身分開啟 PowerShell，把外掛複製到 Windows 標準的 VST3 資料夾：

```powershell
Copy-Item -Recurse -Force "build\GuitarPedalboard_artefacts\Release\VST3\Guitar Pedalboard.vst3" "C:\Program Files\Common Files\VST3\"
```

（在專案資料夾裡執行；用安裝版時勾選 VST3 plugin 就會自動裝好。）

接著在 DAW 重新掃描外掛，新增一個單聲道音軌、開啟錄音監聽 (input monitoring)，掛上 **Guitar Pedalboard** 即可。

## 重新編譯

需要 Visual Studio 2022 (含「使用 C++ 的桌面開發」) 和 Git。JUCE 沒有放在這個儲存庫裡，請先下載本專案使用的版本 9.0.3 到專案資料夾裡的 `JUCE\`。本專案用 VS 內建的 CMake，以下指令都在專案資料夾裡執行：

```powershell
git clone --depth 1 --branch 9.0.3 https://github.com/juce-framework/JUCE.git JUCE
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S . -B build -G "Visual Studio 17 2022" -A x64
& $cmake --build build --config Release
```

或直接用 Visual Studio 開啟 `build\GuitarPedalboard.sln`。網頁版只需要 Node.js：`node web/build.mjs` 會在 `web/dist/` 產生網頁。

### 離線測試

```powershell
build\PedalTest_artefacts\Release\PedalTest.exe test_output
```

會把一段合成的吉他樂句送進每個效果，輸出 WAV 到 `test_output\`，並自動檢查：
- 沒有 NaN / 爆音 / 失控的音量
- 噪音閘能消除 -70 dB 底噪，且不影響正常彈奏
- 每個型號開關、換型號、移動 AMP 方塊時都不會產生爆音 (click)
- 15 個破音型號在預設值時音量差距在 3 dB 內，Drive 從 0 轉到 100% 音量不會暴衝（會印出每個型號的音量表）；50 個音箱頭的音量差距在 4 dB 內
- 每個型號的代號只出現一次、同一類裡沒有同名的型號
- 31 個 Delay / Reverb 型號關掉後尾音會完整地自然消失，其他效果關掉後會立刻安靜
- Hum 濾除能消掉 50 / 60 Hz 哼聲而不影響音符，Denoise 能壓低嘶聲而不影響彈奏
- 每一類效果另有獨立的詳細測試（`Tests/standalone/<類別>_test.cpp`，例如壓縮比、Wah 的共振頻率、延遲時間、殘響長度、音高準確度、音箱頭的失真度），執行方式：`powershell -ExecutionPolicy Bypass -File Tests\standalone\build.ps1 -Name amp`，網頁版對應的比對：`node web/test/standalone.mjs amp AmpFx AMP_MODELS`
- 調音器在 44.1 / 48 / 96 kHz 下的誤差 < 1 cent (實測最大約 0.7 cent)
- EQ 圖上畫的曲線和實際聽到的聲音一致 (誤差 < 0.1 dB)，EQ 歸零時完全透明，拖曳頻段時不會有雜音
- 頻譜分析器對已知頻率的正弦波能顯示在正確位置和音量
- 測試訊號：撥弦的音準誤差 < 0.25 cent；DEMO RIFF 循環無接縫雜音；不同取樣率的音檔播放音高正確
- TAP：穩定 / 不穩的點擊、停頓後重來、突然變速、按鍵彈跳都能正確處理；Reverb pre-delay 時間精準
- 另外會在 `test_output\reference\` 輸出參考資料（含型號清單和一段不斷換型號、開關、移動方塊的腳本），給網頁版的 `node web/test/compare.mjs` 逐樣本比對
- `node web/test/latency.mjs`：量測每個效果增加的延遲（每個破音型號約 0.1–0.25 ms、CAB 約 0.08 ms；整體延遲主要來自音訊介面、瀏覽器和耳機的輸入輸出緩衝）

## 程式碼結構

```
Source/
├─ ParamIDs.h            所有參數的 ID
├─ PluginProcessor.*     參數定義、存檔/讀檔、音訊處理入口
├─ PluginEditor.*        主視窗排版
├─ TestSignalPlayer.h    測試訊號來源 (demo riff / 音檔 / 撥弦)
├─ DSP/
│  ├─ ModelTypes.h       型號資料的格式：類別、旋鈕（範圍、單位、預設值）、TAP 同步
│  ├─ Models.h           型號清單（效果格、音箱頭、音箱），以及每個音箱頭通常搭配的音箱
│  ├─ FxChain.*          訊號鏈：8 個 Slot + AMP / CAB 方塊 + 輸出 EQ + 雜訊處理；換型號 / 開關時的淡入淡出、Delay / Reverb 的 trails
│  ├─ fx/                HD500X 各類效果的引擎（不依賴 JUCE，每個檔案一類）：Dynamics、Mod、Filter、Pitch、Eq、DelayFx、Verb、Wah、Volume、Amp、Cab
│  ├─ NoiseReduction.h   Hum 濾除（50 / 60 Hz 與泛音）與 Denoise（嘶聲抑制）
│  ├─ NoiseGate.h        噪音閘 (含遲滯與 hold，不會斷斷續續)
│  ├─ Distortion.h       HD500X 的 15 種破音型號 (4x oversampling)
│  ├─ Equalizer.h        6 段 EQ (畫面上的曲線也用同一份設計程式)
│  ├─ SpectrumAnalyzer.h 頻譜分析 (FFT)
│  ├─ AudioTap.h         把音訊安全地從音訊執行緒送到畫面 (調音器、頻譜)
│  ├─ Modulation.h       Chorus / Flanger / Phaser / Tremolo (立體聲)
│  ├─ Delay.h            類比風格延遲
│  ├─ ReverbFx.h         殘響
│  ├─ PitchDetector.h    調音器 (YIN 音高偵測演算法)
│  ├─ TestSignal.h       撥弦合成 (Karplus-Strong) 與內建 demo riff
│  ├─ TapTempo.h         TAP 拍速計算與音符長度表
│  └─ DspUtils.h         濾波器、延遲線等基本元件
├─ KnobText.h           旋鈕數值的文字格式
└─ UI/                   訊號鏈 (ChainStrip)、編輯區 (SlotEditor)、TAP、旋鈕、腳踏開關、電平表、調音器、EQ 畫面、輸入來源列
Tests/RenderTest.cpp     離線測試
Tests/UiSnapshot.cpp     不開視窗、不開音效裝置，把外掛畫面存成 PNG（檢查排版用）
Tests/standalone/        各類效果引擎的獨立測試
packaging/               Windows 安裝檔 (Inno Setup) 與免安裝版打包腳本，以及上傳到 GitHub 的腳本（原始碼、Releases）
docs/                    HD500X 型號清單 (hd500x-models.md)、效果引擎的介面規格 (engine-contract.md)
web/src/                 網頁版原始碼：dsp/（C++ DSP 的 JavaScript 移植，每個檔案對應一個 C++ 檔）、worklet.js、app.js、style.css、body.html
web/github/              GitHub 儲存庫首頁的 README 與截圖（deploy-github.ps1 會一起上傳）
web/test/compare.mjs     網頁版 vs C++ 的比對測試
web/test/latency.mjs     每個效果增加的延遲
```

想新增型號：在對應的 `DSP/fx/*.h` 引擎裡加一個變化型和它的型號資料（名稱、原型、旋鈕），網頁版在 `web/src/dsp/` 同名的檔案做同樣的修改；介面會自動長出來，`compare.mjs` 會檢查兩邊的型號清單和聲音是否一致。型號順序會存在設定裡，所以只能往後加。新的一類效果請照 `docs/engine-contract.md` 的介面寫。

## 授權

Copyright (C) 2026 Anyouhuang

本程式（C++ 版與網頁版）是自由軟體，以 **GNU Affero General Public License 第 3 版（AGPLv3）** 授權，全文見 [LICENSE](LICENSE)。可以自由使用、修改和散佈；散佈修改後的版本、或把修改後的版本放在網路上給別人使用時，也必須以同樣的授權公開它的原始碼。本程式不提供任何擔保。

原始碼：https://github.com/Anyouhuang/guitar-pedalboard （`packaging\publish-source.mjs` 會把本機的原始碼更新上去）

使用的第三方元件：

| 元件 | 授權 | 說明 |
|---|---|---|
| [JUCE](https://github.com/juce-framework/JUCE) 9.0.3 | AGPLv3（或 JUCE 商業授權） | 程式框架；本專案採用 AGPLv3 |
| Steinberg ASIO SDK | GPLv3（或 Steinberg 授權） | 隨 JUCE 提供，Windows 版的 ASIO 低延遲支援 |
| Steinberg VST3 SDK | MIT | 隨 JUCE 提供，VST3 外掛格式 |
| Barlow、Barlow Condensed、IBM Plex Mono 字型 | SIL Open Font License | 網頁版從 Google Fonts 載入 |

型號名稱參考 Line 6 POD HD500X 的清單；Line 6、POD 以及各型號原型機器的名稱是各自所有者的商標，本專案與他們沒有任何關係，聲音是自行撰寫的近似模型。
