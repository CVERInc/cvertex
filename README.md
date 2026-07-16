# cvertex

C + vertex。一顆手捲的、零依賴的跨平台遊戲引擎，整包塞得進一片 3.5 吋軟碟（1,474,560 bytes）。

畫面上沒有點陣圖，只有多邊形。

## 現況

Spike 階段。macOS 可跑，Windows / Linux 平台層未寫。

| | bytes |
|---|---|
| macOS 執行檔 | 35,256 |
| 其中我們自己的機器碼（`__text`） | 2,996 |
| 空的 Cocoa 殼（對照組） | 33,592 |

體積幾乎全是 Mach-O 容器開銷，不是引擎。

## 設計

**調色盤索引 framebuffer。** 一個 byte 一個 pixel，全域一張 256 色調色盤。blit 是 memcpy；淡出、閃白、換色都是改調色盤，不碰像素。

**只認多邊形，不認 SVG。** SVG 是創作格式，不是執行格式。build 時把貝茲攤平成折線嚼成二進位；引擎裡沒有 parser。資產管線接 [motifmint](https://oss.cver.net/motifmint)：畫圖 → 追蹤成聚類調色盤的 SVG → 多邊形。

**像素或向量是執行期的一個數字。** 同一份多邊形填進 320×180 就是像素遊戲，填進 1920×1080 就是銳利向量。

**確定性模擬。** 固定時間步長、定點數、無隨機、不讀時鐘。狀態只由輸入改變。免費送 replay（存輸入即存錄影）、lockstep 連線（只交換輸入）、可重播的 debug。

**平台層只回答五個問題。** 給我視窗、給我一塊貼到螢幕的記憶體、鍵盤按了什麼、現在幾點、該收工了嗎。macOS 那份是純 C 直呼 objc runtime，不用 Objective-C 編譯器。

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
