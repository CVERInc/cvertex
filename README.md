# cvertex

C + vertex。一顆手捲的、零依賴的跨平台遊戲引擎，整包塞得進一片 3.5 吋軟碟（1,474,560 bytes）。

畫面上沒有點陣圖，只有多邊形。

## 現況

Spike 階段。macOS 可跑（視窗 + 多邊形 + FM 音效 + 雙人輸入），Windows / Linux 平台層未寫。

| | bytes |
|---|---|
| 空的 Cocoa 殼（對照組） | 33,592 |
| ＋ rasterizer / sim / 平台層 / 輸入 | 35,256 |
| **＋ FM 合成器 / tracker / 音樂** | **35,976** |
| 其中我們自己的機器碼（`__text`） | 5,036 |
| 預算 1,474,560 → 用掉 | **2.43%** |

體積幾乎全是 Mach-O 容器開銷，不是引擎。framework 是 dylib，不進 binary——實測多接一個
AudioToolbox 只要 **72 bytes**，所以音效在體積上等於免費。

## 體積紀律（實測，不是傳說）

**🔴 靜態變數一律零初始化，初值在執行期賦。** 非零初始化 → `__data` → macOS 的
segment 是 16KB 頁對齊 → **4 bytes 的初值硬拖 16,384 bytes 進檔案**。踩過一次：
52,488 → 35,976，只因為刪掉 `static int g_row = -1` 和 `g_running = 1`。
零初始化住 `__bss`（zerofill，不佔硬碟）——320×180 的 framebuffer 一毛錢都沒花。

**每次 build 都印體積帳。** 體積是一等公民，不能等到截止前才發現超了。

**量，不要猜。** 合成器第一版全程削波（peak 32768 / RMS 23772），用聽的抓不到，
量了才發現單 voice 滿檔就溢位。混音增益是算出來的：`>>20`。

## 設計

**調色盤索引 framebuffer。** 一個 byte 一個 pixel，全域一張 256 色調色盤。blit 是 memcpy；淡出、閃白、換色都是改調色盤，不碰像素。

**只認多邊形，不認 SVG。** SVG 是創作格式，不是執行格式。build 時把貝茲攤平成折線嚼成二進位；引擎裡沒有 parser。資產管線接 [motifmint](https://oss.cver.net/motifmint)：畫圖 → 追蹤成聚類調色盤的 SVG → 多邊形。

**像素或向量是執行期的一個數字。** 同一份多邊形填進 320×180 就是像素遊戲，填進 1920×1080 就是銳利向量。

**確定性模擬。** 固定時間步長、定點數、無隨機、不讀時鐘。狀態只由輸入改變。免費送 replay（存輸入即存錄影）、lockstep 連線（只交換輸入）、可重播的 debug。

**平台層只回答五個問題。** 給我視窗、給我一塊貼到螢幕的記憶體、鍵盤按了什麼、現在幾點、該收工了嗎。macOS 那份是純 C 直呼 objc runtime，不用 Objective-C 編譯器。

**聲音是算出來的，不是匯入的。** 沒有 wav、沒有 ogg、沒有解碼器。2-op FM（調變器偏移載波相位，AdLib/OPL2 那味）＋方波／三角／鋸齒／LFSR 雜訊＋ADSR。**一個音色 12 bytes，整首 demo 曲是一張 32×4 的表 = 64 bytes。** sim 只吐 `g_events`，平台層才翻譯成聲音——sim 不認識合成器，所以音訊執行緒污染不到確定性。

## Build

```sh
./build.sh && ./cvertex
```

A/D/W 控制橘色，←/→/↑ 控制藍色，Esc 離開。輸入路由是唯一知道「有幾個玩家」的地方——`sim` 不知道輸入從哪來，所以本地雙人、手把、socket 對它都一樣。

驗確定性：

```sh
./cvertex --headless 3600   # 同輸入必得同 checksum
```

## License

MIT © CVER Inc.
