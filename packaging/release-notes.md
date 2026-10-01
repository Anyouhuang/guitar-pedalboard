Windows 版的電吉他綜合效果器：獨立程式 + VST3 外掛（Windows 10 / 11，64 位元）。和網頁版 https://anyouhuang.github.io/guitar-pedalboard/ 是同一套音效。

## 下載哪一個

| 檔案 | 說明 |
|---|---|
| **GuitarPedalboard-1.2.1-Setup.exe** | 安裝程式（建議）：建立開始功能表捷徑，可選擇同時安裝 VST3 外掛，可以解除安裝 |
| **GuitarPedalboard-1.2.1-Windows-Portable.zip** | 免安裝版：解壓縮後直接執行 `Guitar Pedalboard.exe`；要用 VST3 外掛時，對 `Install-VST3.cmd` 按右鍵「以系統管理員身分執行」 |

- 不需要另外安裝 Visual C++ 執行階段。
- **第一次執行可能出現「Windows 已保護您的電腦」**：因為程式沒有付費的程式碼簽章，按「其他資訊」→「仍要執行」即可。
- 接吉他：吉他接錄音介面，在 **Options → Audio/MIDI Settings** 選 ASIO（延遲最低）、只勾吉他的輸入聲道、取消勾選 Mute audio input。詳細步驟見安裝資料夾裡的 QuickStart-zh-TW.txt。

## 內容

- HD500X 式訊號鏈：8 個效果格 + AMP / CAB 方塊，可自由排序
- 118 種效果、50 種音箱頭、22 種音箱、14 種麥克風
- 調音器（可開關）、6 段 EQ + 頻譜、Hum / Denoise 雜訊處理、TAP 拍速
- 沒有吉他也能試：內建樂句、載入音檔、撥空弦、和弦（Arpeggio / 每拍刷一下的 Strum loop / Single）

## 1.2.1 的變更

- Strum loop 改成每一拍往下刷一次，循環中換和弦會在下一拍換過去
- 網頁版：訊號鏈的方塊點兩下可以開 / 關效果

## 授權

自由軟體，以 AGPLv3 授權，原始碼在這個儲存庫。型號名稱參考 Line 6 POD HD500X 的清單，聲音是自行撰寫的近似模型，與 Line 6 沒有關係。
