# Hardware datasheets

Read these alongside `Schematics.pdf` and sections 2.2 and 5.3 of the [project guide](../CrashNBurn-Project-Guide.pdf). Cite document ID, revision and section/page in the SDD beside each driver sequence, pin assignment and timing limit. These are full manufacturer documents; their copyright and notices remain intact. Retrieved 2026-09-15.

**Flash capacity:** W25Q32JVSSIQ is 32 Mbit = 4 MiB, not 31 MB. Addresses are 0x000000–0x3FFFFF, with 256-byte program pages and 4 KiB erase sectors. The guide explains task queues → data owner → SPI program/read commands → USB export and explicit deletion.

## Winbond W25Q32JV SPI flash

- Local PDF: [W25Q32JV-SPI-Flash.pdf](W25Q32JV-SPI-Flash.pdf) (83 pages).
- Revision: Rev K, April 2026.
- [Manufacturer source](https://www.winbond.com/resource-files/W25Q32JV%20RevK%2004272026%20Plus.pdf).
- SHA-256: `acb6e553940b5d36d6ac8ae0e1a30e58e3c0caec42c4513b43a065fea7150616`.

## Bosch BMP390 barometer

- Local PDF: [BMP390-Barometer.pdf](BMP390-Barometer.pdf) (59 pages).
- Revision: BST-BMP390-DS002-07, revision 1.7, March 2021.
- [Manufacturer source](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmp390-ds002.pdf).
- SHA-256: `8dcf0f74449cb1627e865c7f24d328b76c204d8b9621091ede8c6869fb393e48`.

## Bosch BMI088 accelerometer and gyroscope

- Local PDF: [BMI088-IMU.pdf](BMI088-IMU.pdf) (62 pages).
- Revision: BST-BMI088-DS000-19, revision 1.9, January 2024.
- [Manufacturer source](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi088-ds001.pdf).
- SHA-256: `53ab734dba49ac202fa6182fc2b545df6c6d7459c9fec6e851a1209e6bac417e`.

## ST STM32G491xC/xE MCU family

- Local PDF: [STM32G491-MCU.pdf](STM32G491-MCU.pdf) (197 pages).
- Revision: DS13122 Rev 4, April 2024.
- [Manufacturer source](https://www.st.com/resource/en/datasheet/stm32g491ce.pdf).
- Downloaded from the [RS mirror of the ST document](https://docs.rs-online.com/b4a3/A700000012920120.pdf) after ST download errors; the PDF identifies DS13122 Rev 4 and includes the CET6/LQFP48 variant.
- SHA-256: `978130a7363b37d3149770dc21631ef1f17e2208b2153d67377e7ef35f669056`.

## Reference notes

Winbond viewer page numbers are one greater than its printed page numbers. Rev K has an April 27 cover date and April 28 internal publication footers. Bosch URL basenames differ from the document IDs printed inside the files; cite the internal IDs above. The STM32 family datasheet covers multiple packages: use the STM32G491CET6 LQFP48 pinout/columns for this board.

Record additional MCU reference-manual, errata and CubeG4 HAL package versions in the SDD. Manufacturer updates do not automatically change your reviewed project baseline; review their impact first.
