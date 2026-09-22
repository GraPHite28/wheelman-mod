using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;

// Builds MapData\roads.bin from the city map picture: a 1536x1024 grid (one cell = 4x4 map pixels) marking road cells.
class RoadMask
{
    const int W = 6144, H = 4096, CW = W / 4, CH = H / 4;
    static bool[] road = new bool[W * H];

    static void Load(string path, int ox)
    {
        var bmp = new Bitmap(path);
        var bd = bmp.LockBits(new Rectangle(0, 0, bmp.Width, bmp.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        byte[] px = new byte[bd.Stride * bmp.Height];
        System.Runtime.InteropServices.Marshal.Copy(bd.Scan0, px, 0, px.Length);
        for (int y = 0; y < bmp.Height; y++)
            for (int x = 0; x < bmp.Width; x++)
            {
                int o = y * bd.Stride + x * 4;
                int b = px[o], g = px[o + 1], r = px[o + 2];
                int mx = Math.Max(r, Math.Max(g, b)), mn = Math.Min(r, Math.Min(g, b));
                road[y * W + ox + x] = mx - mn < 30 && mx > 165;
            }
        bmp.UnlockBits(bd);
    }

    internal static void Main(string[] a)
    {
        string d = a[0];   // folder with the extracted L.png / R.png
        Load(d + "\\L.png", 0);
        Load(d + "\\R.png", 4096);
        bool[] cell = new bool[CW * CH];
        for (int cy = 0; cy < CH; cy++)
            for (int cx = 0; cx < CW; cx++)
            {
                int n = 0;
                for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) if (road[(cy * 4 + y) * W + cx * 4 + x]) n++;
                cell[cy * CW + cx] = n >= 5;
            }
        // close 1-cell gaps
        bool[] c2 = (bool[])cell.Clone();
        for (int y = 1; y < CH - 1; y++)
            for (int x = 1; x < CW - 1; x++)
            {
                int i = y * CW + x;
                if (cell[i]) continue;
                if ((cell[i - 1] && cell[i + 1]) || (cell[i - CW] && cell[i + CW])) c2[i] = true;
            }
        cell = c2;
        // largest 8-connected component
        int[] label = new int[CW * CH]; int best = 0, bestSize = 0, cur = 0;
        var stack = new Stack<int>();
        for (int i = 0; i < cell.Length; i++)
        {
            if (!cell[i] || label[i] != 0) continue;
            cur++; int size = 0; stack.Push(i); label[i] = cur;
            while (stack.Count > 0)
            {
                int p = stack.Pop(); size++;
                int px = p % CW, py = p / CW;
                for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
                {
                    int nx = px + dx, ny = py + dy;
                    if (nx < 0 || ny < 0 || nx >= CW || ny >= CH) continue;
                    int q = ny * CW + nx;
                    if (cell[q] && label[q] == 0) { label[q] = cur; stack.Push(q); }
                }
            }
            if (size > bestSize) { bestSize = size; best = cur; }
        }
        Console.WriteLine("components " + cur + " largest " + bestSize + " of " + Array.FindAll(cell, v => v).Length);
        byte[] outb = new byte[12 + CW * CH];
        System.Text.Encoding.ASCII.GetBytes("ROAD").CopyTo(outb, 0);
        BitConverter.GetBytes(CW).CopyTo(outb, 4); BitConverter.GetBytes(CH).CopyTo(outb, 8);
        for (int i = 0; i < cell.Length; i++) outb[12 + i] = (byte)(label[i] == best ? 1 : 0);
        Directory.CreateDirectory(a[1]);
        File.WriteAllBytes(a[1] + "\\roads.bin", outb);
    }
}
