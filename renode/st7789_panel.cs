//
// 哑 ST7789 (SPI 从设备): 只统计收到的字节流, 不解析命令、不画画面
// 目的: 在 Renode 里确认 "固件 → SPI2 → 外设" 这条链通不通、一次刷屏送了多少字节
// 接入: emulation.resc 里加        include @./renode/st7789_panel.cs
//       emulation_peripheral.repl 里加  st7789_panel: St7789Dummy @ spi2
// 备注: 若编译报找不到 ISPIPeripheral, 把它换成 ISpiPeripheral 再试 (Renode 各版本命名有差异)
//
using Antmicro.Renode.Core;
using Antmicro.Renode.Logging;
using Antmicro.Renode.Peripherals.SPI;

namespace Antmicro.Renode.Peripherals
{
    public class St7789Dummy : ISPIPeripheral
    {
        public St7789Dummy()
        {
            this.Log(LogLevel.Info, "ST7789 dummy attached: SPI 从设备, 只统计字节流");
        }

        /* 每收一个 MOSI 字节调一次 */
        public byte Transmit(byte data)
        {
            if(txBytes == 0)
            {
                firstByte = data;
            }
            txBytes++;
            lastByte = data;
            return 0;
        }

        /* 片选抬起 = 一次传输结束 (软件片选时由控制器在传输前后调) */
        public void FinishTransmission()
        {
            transaction++;
            totalBytes += txBytes;

            /* 前 40 次传输全打, 之后每 100 次打一行, 免得刷爆控制台 */
            if(transaction <= 40 || (transaction % 100) == 0)
            {
                this.Log(LogLevel.Info, "tx#{0}: {1} bytes, first=0x{2:X2} last=0x{3:X2}, total={4}",
                         transaction, txBytes, firstByte, lastByte, totalBytes);
            }
            txBytes = 0;
        }

        public void Reset()
        {
            txBytes = 0;
            transaction = 0;
            totalBytes = 0;
        }

        private int txBytes;
        private int transaction;
        private long totalBytes;
        private byte firstByte;
        private byte lastByte;
    }
}
