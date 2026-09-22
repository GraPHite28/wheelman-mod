using System;
using System.IO;

// Rebuilds an uncompressed UE3 package from a chunk-compressed (LZO1X)
// Wheelman package: places each decompressed chunk at its uncompressed
// offset and zeroes CompressionFlags/chunk count in the header.
class Decomp
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
                    if (t >= 64)
                    {
                        mpos = op - 1; mpos -= (t >> 2) & 7; mpos -= src[ip++] << 3; t = (t >> 5) - 1; st = COPY;
                    }
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
                    else
                    {
                        mpos = op - 1; mpos -= t >> 2; mpos -= src[ip++] << 2;
                        dst[op++] = dst[mpos++]; dst[op++] = dst[mpos];
                        st = MATCH_DONE;
                    }
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

    internal static void Main(string[] args)
    {
        byte[] b = File.ReadAllBytes(args[0]);
        int flagsPos = 101, countPos = 105;
        int flags = BitConverter.ToInt32(b, flagsPos), count = BitConverter.ToInt32(b, countPos);
        if (flags == 0) { File.WriteAllBytes(args[1], b); Console.WriteLine("not compressed, copied"); return; }
        if (count <= 0 || count > 4096) throw new Exception("unexpected chunk count " + count + " (flags " + flags + ")");
        int total = 0;
        var ch = new int[count, 4];
        for (int i = 0; i < count; i++)
        {
            for (int k = 0; k < 4; k++) ch[i, k] = BitConverter.ToInt32(b, countPos + 4 + i * 16 + k * 4);
            total = Math.Max(total, ch[i, 0] + ch[i, 1]);
        }
        byte[] outp = new byte[total];
        Array.Copy(b, outp, Math.Min(113, b.Length));
        BitConverter.GetBytes(0).CopyTo(outp, flagsPos);
        BitConverter.GetBytes(0).CopyTo(outp, countPos);
        for (int i = 0; i < count; i++)
        {
            int uOff = ch[i, 0], uSize = ch[i, 1], cOff = ch[i, 2], cSize = ch[i, 3];
            uint tag = BitConverter.ToUInt32(b, cOff);
            int blockSize = BitConverter.ToInt32(b, cOff + 4);
            int totComp = BitConverter.ToInt32(b, cOff + 8), totUnc = BitConverter.ToInt32(b, cOff + 12);
            if (tag != 0x9E2A83C1 || totUnc != uSize) throw new Exception("bad chunk header " + i);
            int nBlocks = (totUnc + blockSize - 1) / blockSize;
            int dataPos = cOff + 16 + nBlocks * 8, dstPos = uOff;
            for (int k = 0; k < nBlocks; k++)
            {
                int bc = BitConverter.ToInt32(b, cOff + 16 + k * 8), bu = BitConverter.ToInt32(b, cOff + 20 + k * 8);
                int got = Lzo1x(b, dataPos, dataPos + bc, outp, dstPos, dstPos + bu);
                if (got != bu) throw new Exception("chunk " + i + " block " + k + ": got " + got + " expected " + bu);
                dataPos += bc; dstPos += bu;
            }
        }
        File.WriteAllBytes(args[1], outp);
        Console.WriteLine("ok, wrote " + args[1] + " (" + outp.Length + " bytes)");
    }
}
