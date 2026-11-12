# ViTube TH

**ViTube ที่อ่านภาษาไทยได้** — แอป YouTube สำหรับ PlayStation Vita
ฉบับนี้ fork มาจาก [**ViTube** ของ shorelight82](https://github.com/shorelight82/vitube-vpk)
และเพิ่มเฉพาะการแสดงผลภาษาไทย

> ตัวแอปทั้งหมดเป็นผลงานของ **shorelight82** ได้แก่ หน้าแรก การค้นหา
> ช่องที่ติดตาม การล็อกอิน การเล่นวิดีโอ 720p ด้วยฮาร์ดแวร์ของ Vita และระบบเครือข่าย
> fork นี้แก้เฉพาะส่วนที่ทำให้ตัวอักษรไทยแสดงได้ถูกต้อง
> ถ้าชอบแอปนี้ กรุณาไปกดดาวให้
> [repo ต้นทาง](https://github.com/shorelight82/vitube-vpk) ด้วยครับ

## ดาวน์โหลด

**[⬇ ดาวน์โหลด ViTube-TH.vpk (เวอร์ชันล่าสุด)](../../releases/latest/download/ViTube-TH.vpk)**
· [ดู Release ทั้งหมด](../../releases)

### วิธีติดตั้ง

1. Vita ต้องลง HENkaku/Ensō ไว้แล้ว
2. โอนไฟล์ `ViTube-TH.vpk` เข้า Vita ผ่าน VitaShell (FTP หรือ USB)
3. ใน VitaShell กด ✕ ที่ไฟล์เพื่อติดตั้ง
4. เปิดแอป ไปที่แท็บ **More → Region → ประเทศไทย**

**หมายเหตุ:** แอปนี้ใช้ Title ID เดียวกับ ViTube ต้นฉบับ (`VTUB00001`)
จึงติดตั้งทับ ViTube ตัวเดิม การล็อกอินและประวัติที่เก็บใน `ux0:data/ViTube/`
ยังอยู่ครบ ถ้าอยากกลับไปใช้ตัวต้นฉบับ ให้ลง VPK จาก
[Release ของต้นทาง](https://github.com/shorelight82/vitube-vpk/releases) ทับได้เลย

ถ้าอัปเดตมาจาก ViTube เวอร์ชัน 1.2 หรือเก่ากว่า ให้รีบูตเครื่องหนึ่งครั้งหลังติดตั้ง
(รายละเอียดอยู่ใน [README ต้นฉบับ](README.upstream.md#installing-on-the-vita))

## สิ่งที่ fork นี้เพิ่ม

| ก่อน | หลัง |
|---|---|
| ชื่อคลิปและชื่อช่องภาษาไทยขึ้นเป็น □□□□ | แสดงเป็นตัวอักษรไทย |
| สระบนกับวรรณยุกต์ซ้อนกันจนอ่านยาก เช่น "ที่" กลายเป็น "ทีี" | วางตำแหน่งตามแบบที่ HarfBuzz จัดให้ |
| ไม่มีภูมิภาคไทย | เลือก **ประเทศไทย** ได้ ทำให้หน้าแรก ผลค้นหา และคำบอกวันที่เป็นภาษาไทย |
| เรียงคลิป "ใหม่ล่าสุดก่อน" ไม่ได้กับคำอย่าง "3 วันที่ผ่านมา" | อ่านคำบอกเวลาภาษาไทยได้ |

### ข้อจำกัดที่ยังมีอยู่

- **เมนูของแอปยังเป็นภาษาอังกฤษ** เพราะยังไม่มีคำแปลไทย เมื่อเลือกภูมิภาคไทย
  แอปจะใช้เมนูภาษาอังกฤษ ส่วนเนื้อหาจาก YouTube จะเป็นภาษาไทย
- **การพิมพ์ค้นหา**ใช้คีย์บอร์ดของระบบ Vita
  ซึ่งพิมพ์ภาษาไทยได้หรือไม่ขึ้นอยู่กับคีย์บอร์ดของเครื่อง
- ฟอนต์ไทยรองรับพยัญชนะตามด้วยสระบนหรือล่าง วรรณยุกต์ และสระอำ ซึ่งครอบคลุมภาษาไทยทั่วไป
  ถ้าเครื่องหมายซ้อนกันมากกว่านั้น (เช่น พิมพ์วรรณยุกต์ซ้ำ) จะวาดตามลำดับโดยไม่จัดตำแหน่ง

พบปัญหาเรื่องการแสดงผลภาษาไทย แจ้งได้ที่ [Issues ของ fork นี้](../../issues)
ปัญหาอื่นของแอป (เล่นวิดีโอ ล็อกอิน ค้นหา) ส่วนใหญ่มาจากต้นทาง
ให้ดู [Issues ของ ViTube](https://github.com/shorelight82/vitube-vpk/issues)

## ทำงานอย่างไร (สำหรับนักพัฒนา)

แอปวาดตัวอักษรด้วย vita2d ซึ่งวาดทีละตัวโดยไม่มีระบบจัดวางตัวอักษร (shaping)
และไม่อ่านตาราง GSUB/GPOS ในฟอนต์ ขณะที่ภาษาไทยต้องใช้ตารางเหล่านี้
เพื่อยกวรรณยุกต์ขึ้นเหนือสระบน เลื่อนเครื่องหมายบน ป ฝ ฟ ไปทางซ้าย
และตัดเชิง ญ ฐ เมื่อมีสระล่าง

fork นี้จึงย้ายการ shaping ไปทำตอน build แทนตอนรันแอป:

1. [`tools/make-thai-font.py`](tools/make-thai-font.py) ใช้ HarfBuzz จัดวางทุกกลุ่มอักษร
   (พยัญชนะ + สระบน/ล่าง + วรรณยุกต์ + สระอำ) ของ Noto Sans Thai
   ผลที่ต่างจากการวาดทีละตัวจะถูกบันทึกเป็น glyph ประกอบในช่วง Private Use Area
   ของ [`assets/thai.ttf`](assets/thai.ttf) (ปัจจุบันมี 2,268 กลุ่ม)
   และสคริปต์จะสร้างตาราง [`src/ui/thai_shape_table.h`](src/ui/thai_shape_table.h) ไปพร้อมกัน
2. ตอนรันแอป [`src/ui/thai_shape.c`](src/ui/thai_shape.c)
   จะเปลี่ยนข้อความไทยให้เป็น code point ของ glyph เหล่านั้น
   ทำให้ vita2d วาดออกมาถูกต้องโดยไม่ต้องจัดตำแหน่งเพิ่ม
   ผลลัพธ์ไม่ยาวกว่าข้อความเดิม จึงใช้บัฟเฟอร์ขนาดเท่าข้อความเดิมได้
3. [`src/ui/ui.c`](src/ui/ui.c) ใช้ `thai.ttf` เป็นฟอนต์สำรองตัวที่สาม
   ต่อจาก DejaVu Sans และ Noto Emoji ที่ต้นทางใช้อยู่แล้ว
   และขยายกรอบตัดข้อความแนวตั้ง เพื่อไม่ให้วรรณยุกต์ที่ซ้อนสูงหรือสระล่างโดนตัด

ส่วนอื่นที่แก้: เพิ่มภูมิภาค `th/TH` ใน [`src/yt/region.c`](src/yt/region.c)
และเพิ่มคำบอกเวลาภาษาไทยใน `age_minutes()` ของ [`src/yt/parse.c`](src/yt/parse.c)

ถ้าต้องการสร้างฟอนต์ใหม่ (เช่น ตอนเปลี่ยนเวอร์ชัน Noto Sans Thai):

```sh
pip install fonttools uharfbuzz
python3 tools/make-thai-font.py NotoSansThai-Regular.ttf
```

ต้องสร้าง `assets/thai.ttf` และ `src/ui/thai_shape_table.h` ใหม่พร้อมกันเสมอ
URL และ sha256 ของฟอนต์ต้นฉบับอยู่ในหัวไฟล์สคริปต์

### Unit test

```sh
gcc -Isrc -Wall -o /tmp/tt tests/test_thai_shape.c src/ui/thai_shape.c && /tmp/tt
```

test ตรวจทุกแถวในตารางที่สร้างไว้ ทั้งเมื่อข้อความอยู่ติดกับข้อความอื่น
ข้อความที่ไม่ใช่ภาษาไทยต้องผ่านไปโดยไม่เปลี่ยน
UTF-8 ที่ถูกตัดกลางคันต้องไม่ทำให้อ่านเกินขอบ
และผลลัพธ์ต้องไม่ยาวกว่าข้อความเดิม
test อื่นของต้นทางดูได้ใน [README ต้นฉบับ](README.upstream.md#tests)

## Build

ทุกครั้งที่ push จะมี [GitHub Actions](.github/workflows/build.yml)
build VPK ด้วย Docker image ของ VitaSDK แล้วแนบไฟล์ไว้ใน artifact ของ run นั้น
ถ้า push tag ที่ขึ้นต้นด้วย `v` จะสร้าง Release พร้อมไฟล์ `ViTube-TH.vpk` ให้ด้วย

ถ้าจะ build เอง ทำตามขั้นตอนใน [README ต้นฉบับ](README.upstream.md#build) ได้เลย
(ต้องมี VitaSDK และ build FFmpeg กับ curl ก่อน)

## เครดิต

- **[ViTube](https://github.com/shorelight82/vitube-vpk)** โดย **shorelight82**
  เป็นตัวแอปทั้งหมด รวมถึงสถาปัตยกรรม การเชื่อมต่อ YouTube innertube
  การเล่น HLS 720p การถอดรหัสวิดีโอด้วยฮาร์ดแวร์ ระบบเครือข่าย vita-https
  และ UI ทั้งหมด
  เอกสารฉบับเต็มของผู้พัฒนาอยู่ใน [README.upstream.md](README.upstream.md)
- **[Noto Sans Thai](https://github.com/notofonts/thai)** โดย The Noto Project Authors
  ใช้ภายใต้ SIL Open Font License 1.1
  ([`assets/THAI-FONT-LICENSE.txt`](assets/THAI-FONT-LICENSE.txt))
  `assets/thai.ttf` คือฉบับดัดแปลงที่เพิ่ม glyph ซึ่งจัดตำแหน่งไว้แล้ว
- **[HarfBuzz](https://github.com/harfbuzz/harfbuzz)** และ
  **[fontTools](https://github.com/fonttools/fonttools)**
  ใช้ตอนสร้างฟอนต์เท่านั้น ไม่ได้รวมอยู่ในแอป
- ส่วนประกอบอื่นที่ต้นทางใช้ เช่น FFmpeg, ส่วนถอดรหัส `h264_vita` จาก wiliwili,
  VitaWave, curl, Mbed TLS, jansson, DejaVu Sans, Noto Emoji และ VitaSDK
  ดูรายการเต็มและไลเซนส์ได้ที่ [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)

## ไลเซนส์

GPL-3.0-only เหมือนต้นทาง (ดู [`LICENSE`](LICENSE))
ซอร์สโค้ดที่ตรงกับไฟล์ VPK แต่ละ Release คือ repo นี้ที่ tag ของ Release นั้น
(ตามข้อกำหนด GPL-3.0 §6)
ไลเซนส์ของส่วนประกอบทั้งหมดรวมอยู่ในไฟล์ VPK ที่โฟลเดอร์ `licenses/`
