Windows 版的電吉他綜合效果器：獨立程式 + VST3 外掛（Windows 10 / 11，64 位元）。和網頁版 https://anyouhuang.github.io/guitar-pedalboard/ 是同一套音效。

## 下載哪一個

| 檔案 | 說明 |
|---|---|
| **GuitarPedalboard-1.3.0-Setup.exe** | 安裝程式（建議）：建立開始功能表捷徑，可選擇同時安裝 VST3 外掛，可以解除安裝 |
| **GuitarPedalboard-1.3.0-Windows-Portable.zip** | 免安裝版：解壓縮後直接執行 `Guitar Pedalboard.exe`；要用 VST3 外掛時，對 `Install-VST3.cmd` 按右鍵「以系統管理員身分執行」 |

- 不需要另外安裝 Visual C++ 執行階段。
- **第一次執行可能出現「Windows 已保護您的電腦」**：因為程式沒有付費的程式碼簽章，按「其他資訊」→「仍要執行」即可。
- 接吉他：吉他接錄音介面，在 **Options → Audio/MIDI Settings** 選 ASIO（延遲最低；沒有 ASIO 驅動可以選 Windows Audio (Exclusive Mode) 或 (Low Latency Mode)）、Audio buffer size 設 64–256、只勾吉他的輸入聲道、取消勾選 Mute audio input。詳細步驟見安裝資料夾裡的 QuickStart-zh-TW.txt。

## 1.3.0 的變更：降低延遲

- **Windows 版**：標題下方顯示目前的音訊驅動、緩衝大小和驅動回報的來回延遲（綠燈 15 ms 以下、黃燈 30 ms 以下、紅燈超過 30 ms），點它直接開啟音訊設定。
- **網頁版**：新增「延遲模式」，預設「低延遲」：向瀏覽器要求裝置最小的輸出緩衝，並把每次處理的區塊縮小到 64 個取樣（Chrome / Edge 153 以上）；聲音斷續時可改成「穩定」。「音訊狀態」會顯示輸出／輸入延遲和斷音次數。效果取決於音效裝置的驅動。
- **網頁版**：延遲線的讀取不再於每個取樣產生暫時物件，預設效果鏈在音訊執行緒上產生的暫時記憶體減少約八成，運算快約一成。聲音與之前逐樣本相同。
- 新的開發工具：`AudioProbe`（列出各種 Windows 音訊驅動類型的緩衝大小與延遲）、`web/test/alloc.mjs`（量測音訊執行緒的記憶體配置）。

## 內容

- HD500X 式訊號鏈：8 個效果格 + AMP / CAB 方塊，可自由排序
- 118 種效果、50 種音箱頭、22 種音箱、14 種麥克風
- 調音器（可開關）、6 段 EQ + 頻譜、Hum / Denoise 雜訊處理、TAP 拍速
- 沒有吉他也能試：內建樂句、載入音檔、撥空弦、和弦（Arpeggio / 每拍刷一下的 Strum loop / Single）

## 授權

自由軟體，以 AGPLv3 授權，原始碼在這個儲存庫。型號名稱參考 Line 6 POD HD500X 的清單，聲音是自行撰寫的近似模型，與 Line 6 沒有關係。
