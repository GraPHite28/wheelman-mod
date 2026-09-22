using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;

// Extracts a cooked Wheelman Texture2D (top mip) to PNG. The texture's bulk data is stored as one LZO1X chunk with the
// package chunk header (tag 0x9E2A83C1); it is found by scanning the export data for the tag.
// Usage: TexDump <package.upk> <exportOffset> <exportSize> <sizeX> <sizeY> <format 5=DXT1 7=DXT5 6=DXT3> <out.png>
class TexDump
{
    const int MATCH = 2, TOP = 0, FIRST_LIT = 1, COPY = 5, MATCH_DONE = 3, MATCH_NEXT = 4;

    static int Lzo1x(byte[] src, int ip, int ipEnd, byte[] dst, int op, int opEnd)
    {
        int t = 0, mpos = 0, st;
        int start = op;
        if (src[ip] > 17)
        {
            t = src[ip++] - 17;
            if (t < 4) st = MATCH_NEXT;
            else { do dst[op++] = src[ip++]; while (--t > 0); st = FIRST_LIT; }
        }
        else st = TOP;
        while (true)
        {
            switch (st)
            {
                case TOP:
                    t = src[ip++];
                    if (t >= 16) { st = MATCH; break; }
                    if (t == 0) { while (src[ip] == 0) { t += 255; ip++; } t += 15 + src[ip++]; }
                    dst[op++] = src[ip++]; dst[op++] = src[ip++]; dst[op++] = src[ip++];
                    do dst[op++] = src[ip++]; while (--t > 0);
                    st = FIRST_LIT; break;
                case FIRST_LIT:
                    t = src[ip++];
                    if (t >= 16) { st = MATCH; break; }
                    mpos = op - (1 + 0x0800);
                    mpos -= t >> 2; mpos -= src[ip++] << 2;
                    dst[op++] = dst[mpos++]; dst[op++] = dst[mpos++]; dst[op++] = dst[mpos];
                    st = MATCH_DONE; break;
                case MATCH:
                    if (t >= 64) { mpos = op - 1; mpos -= (t >> 2) & 7; mpos -= src[ip++] << 3; t = (t >> 5) - 1; st = COPY; }
                    else if (t >= 32)
                    {
                        t &= 31;
                        if (t == 0) { while (src[ip] == 0) { t += 255; ip++; } t += 31 + src[ip++]; }
                        mpos = op - 1; mpos -= (src[ip] >> 2) + (src[ip + 1] << 6); ip += 2; st = COPY;
                    }
                    else if (t >= 16)
                    {
                        mpos = op; mpos -= (t & 8) << 11;
                        t &= 7;
                        if (t == 0) { while (src[ip] == 0) { t += 255; ip++; } t += 7 + src[ip++]; }
                        mpos -= (src[ip] >> 2) + (src[ip + 1] << 6); ip += 2;
                        if (mpos == op) return op - start;
                        mpos -= 0x4000; st = COPY;
                    }
                    else { mpos = op - 1; mpos -= t >> 2; mpos -= src[ip++] << 2; dst[op++] = dst[mpos++]; dst[op++] = dst[mpos]; st = MATCH_DONE; }
                    break;
                case COPY:
                    dst[op++] = dst[mpos++]; dst[op++] = dst[mpos++];
                    do dst[op++] = dst[mpos++]; while (--t > 0);
                    st = MATCH_DONE; break;
                case MATCH_DONE:
                    t = src[ip - 2] & 3;
                    st = t == 0 ? TOP : MATCH_NEXT; break;
                case MATCH_NEXT:
                    dst[op++] = src[ip++];
                    if (t > 1) { dst[op++] = src[ip++]; if (t > 2) dst[op++] = src[ip++]; }
                    t = src[ip++]; st = MATCH; break;
            }
            if (op > opEnd) throw new Exception("output overrun");
        }
    }

    static uint Rgb565(int c, out int r, out int g, out int b)
    {
        r = ((c >> 11) & 31) * 255 / 31; g = ((c >> 5) & 63) * 255 / 63; b = (c & 31) * 255 / 31;
        return 0;
    }

    internal static void Main(string[] a)
    {
        byte[] pk = File.ReadAllBytes(a[0]);
        int off = int.Parse(a[1]), size = int.Parse(a[2]), w = int.Parse(a[3]), h = int.Parse(a[4]), fmt = int.Parse(a[5]); bool auto = w == 0;
        int tag = -1;
        for (int i = off; i + 16 < off + size; i++)
            if (pk[i] == 0xC1 && pk[i + 1] == 0x83 && pk[i + 2] == 0x2A && pk[i + 3] == 0x9E) { tag = i; break; }
        if (tag < 0) throw new Exception("no compressed chunk tag found");
        int blockSize = BitConverter.ToInt32(pk, tag + 4), totComp = BitConverter.ToInt32(pk, tag + 8), totUnc = BitConverter.ToInt32(pk, tag + 12);
        int nBlocks = (totUnc + blockSize - 1) / blockSize;
        byte[] raw = new byte[totUnc];
        int dataPos = tag + 16 + nBlocks * 8, dst = 0;
        for (int k = 0; k < nBlocks; k++)
        {
            int bc = BitConverter.ToInt32(pk, tag + 16 + k * 8), bu = BitConverter.ToInt32(pk, tag + 20 + k * 8);
            int got = Lzo1x(pk, dataPos, dataPos + bc, raw, dst, dst + bu);
            if (got != bu) throw new Exception("block " + k + " got " + got + " expected " + bu);
            dataPos += bc; dst += bu;
        }
        Console.WriteLine("decompressed " + totUnc + " bytes");
        if (auto) { w = BitConverter.ToInt32(pk, dataPos); h = BitConverter.ToInt32(pk, dataPos + 4); fmt = totUnc == w * h / 2 ? 5 : (totUnc == w * h ? 7 : 0); Console.WriteLine("auto " + w + "x" + h + " fmt " + fmt); if (fmt == 0) return; }
        if (a.Length > 7) File.WriteAllBytes(a[7], raw);   // optional: the raw block-compressed top mip (used for the map texture)
        int blockBytes = fmt == 5 ? 8 : 16;
        int expected = (w / 4) * (h / 4) * blockBytes;
        Console.WriteLine("expected top mip " + expected + " bytes, w=" + w + " h=" + h);
        // if the chunk holds more than the top mip, use the first expected bytes; if less, the stored mip is smaller
        while (expected > totUnc && w > 4) { w /= 2; h /= 2; expected = (w / 4) * (h / 4) * blockBytes; }
        var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb);
        var bd = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
        byte[] px = new byte[bd.Stride * h];
        int bw = w / 4, bh = h / 4;
        int[] rr = new int[4], gg = new int[4], bb = new int[4], aa = new int[4];
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++)
            {
                int p = (by * bw + bx) * blockBytes;
                int c0 = raw[p + blockBytes - 8] | (raw[p + blockBytes - 7] << 8), c1 = raw[p + blockBytes - 6] | (raw[p + blockBytes - 5] << 8);
                uint idx = BitConverter.ToUInt32(raw, p + blockBytes - 4);
                int r0, g0, b0, r1, g1, b1;
                Rgb565(c0, out r0, out g0, out b0); Rgb565(c1, out r1, out g1, out b1);
                rr[0] = r0; gg[0] = g0; bb[0] = b0; rr[1] = r1; gg[1] = g1; bb[1] = b1;
                aa[0] = aa[1] = aa[2] = aa[3] = 255;
                if (c0 > c1 || fmt != 5)
                {
                    rr[2] = (2 * r0 + r1) / 3; gg[2] = (2 * g0 + g1) / 3; bb[2] = (2 * b0 + b1) / 3;
                    rr[3] = (r0 + 2 * r1) / 3; gg[3] = (g0 + 2 * g1) / 3; bb[3] = (b0 + 2 * b1) / 3;
                }
                else
                {
                    rr[2] = (r0 + r1) / 2; gg[2] = (g0 + g1) / 2; bb[2] = (b0 + b1) / 2;
                    rr[3] = 0; gg[3] = 0; bb[3] = 0; aa[3] = 0;
                }
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++)
                    {
                        int ci = (int)((idx >> (2 * (y * 4 + x))) & 3);
                        int alpha = aa[ci];
                        if (fmt == 7)
                        {
                            int a0 = raw[p], a1 = raw[p + 1];
                            ulong bits = 0;
                            for (int q = 0; q < 6; q++) bits |= (ulong)raw[p + 2 + q] << (8 * q);
                            int ai = (int)((bits >> (3 * (y * 4 + x))) & 7);
                            int av;
                            if (a0 > a1) av = ai == 0 ? a0 : ai == 1 ? a1 : ((8 - ai) * a0 + (ai - 1) * a1) / 7;
                            else av = ai == 0 ? a0 : ai == 1 ? a1 : ai == 6 ? 0 : ai == 7 ? 255 : ((6 - ai) * a0 + (ai - 1) * a1) / 5;
                            alpha = av;
                        }
                        else if (fmt == 6)
                        {
                            int nib = (raw[p + (y * 4 + x) / 2] >> (4 * ((y * 4 + x) & 1))) & 15;
                            alpha = nib * 17;
                        }
                        int o = (by * 4 + y) * bd.Stride + (bx * 4 + x) * 4;
                        px[o] = (byte)bb[ci]; px[o + 1] = (byte)gg[ci]; px[o + 2] = (byte)rr[ci]; px[o + 3] = (byte)alpha;
                    }
            }
        System.Runtime.InteropServices.Marshal.Copy(px, 0, bd.Scan0, px.Length);
        bmp.UnlockBits(bd);
        bmp.Save(a[6], ImageFormat.Png);
        Console.WriteLine("wrote " + a[6] + " " + w + "x" + h);
    }
}

