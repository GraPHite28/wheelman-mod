using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;

// Renders the first frame of named sprites of a Scaleform (GFx) SWF to PNG files. Only what the map marker sprites of
// UI_PDA need: DefineShape 1-4 (solid, linear gradient, bitmap fills, strokes), DefineSprite, PlaceObject2 (matrix, colour
// transform, first frame only) and GFx external images (tag 1001 -> the extracted atlas PNGs).
// Usage: SwfIcons <pda.swf (decompressed body)> <atlas png dir> <out dir> <size> name=charId ...
class SwfIcons
{
    class BitReader
    {
        public byte[] d; public int pos; int bit;
        public BitReader(byte[] data, int p) { d = data; pos = p; bit = 0; }
        public void Align() { if (bit != 0) { bit = 0; pos++; } }
        public uint UB(int n) { uint v = 0; for (int i = 0; i < n; i++) { v = (v << 1) | (uint)((d[pos] >> (7 - bit)) & 1); if (++bit == 8) { bit = 0; pos++; } } return v; }
        public int SB(int n) { if (n == 0) return 0; uint v = UB(n); if ((v & (1u << (n - 1))) != 0) return (int)v - (1 << n); return (int)v; }
        public int U8() { Align(); return d[pos++]; }
        public int U16() { Align(); int v = d[pos] | (d[pos + 1] << 8); pos += 2; return v; }
        public int S16() { return (short)U16(); }
        public uint U32() { Align(); uint v = BitConverter.ToUInt32(d, pos); pos += 4; return v; }
    }

    class Edge { public int x0, y0, cx, cy, x1, y1; public bool curve; public int f0, f1, line; }
    class Fill { public int type; public Color color; public Matrix m; public int bmp; public List<KeyValuePair<int, Color>> stops = new List<KeyValuePair<int, Color>>(); }
    class Group { public List<Fill> fills = new List<Fill>(); public List<KeyValuePair<int, Color>> lines = new List<KeyValuePair<int, Color>>(); public List<Edge> edges = new List<Edge>(); }
    class Shape { public int minx, miny, maxx, maxy; public List<Group> groups = new List<Group>(); }
    class Place { public int depth, chr = -1; public Matrix m = new Matrix(); public float[] cx; }
    class Sprite { public List<Place> frame1 = new List<Place>(); }
    class CX { public float rm = 1, gm = 1, bm = 1, am = 1; public int ra, ga, ba, aa; }

    static Dictionary<int, Shape> shapes = new Dictionary<int, Shape>();
    static Dictionary<int, byte[]> sprites = new Dictionary<int, byte[]>();
    static Dictionary<int, Bitmap> images = new Dictionary<int, Bitmap>();
    static byte[] body;

    static Matrix ReadMatrix(BitReader r)
    {
        r.Align();
        float sx = 1, sy = 1, r0 = 0, r1 = 0;
        if (r.UB(1) == 1) { int n = (int)r.UB(5); sx = r.SB(n) / 65536f; sy = r.SB(n) / 65536f; }
        if (r.UB(1) == 1) { int n = (int)r.UB(5); r0 = r.SB(n) / 65536f; r1 = r.SB(n) / 65536f; }
        int nt = (int)r.UB(5); int tx = r.SB(nt), ty = r.SB(nt);
        r.Align();
        return new Matrix(sx, r0, r1, sy, tx, ty);
    }

    static Color ReadColor(BitReader r, bool alpha)
    {
        int R = r.U8(), G = r.U8(), B = r.U8(); int A = alpha ? r.U8() : 255;
        return Color.FromArgb(A, R, G, B);
    }

    static Fill ReadFill(BitReader r, int shapeType)
    {
        var f = new Fill(); f.type = r.U8();
        bool alpha = shapeType >= 3;
        if (f.type == 0) f.color = ReadColor(r, alpha);
        else if (f.type == 0x10 || f.type == 0x12 || f.type == 0x13)
        {
            f.m = ReadMatrix(r);
            r.Align();
            int n;
            if (shapeType == 4) { int b = r.U8(); n = b & 15; } else n = r.U8();
            for (int i = 0; i < n; i++) { int ratio = r.U8(); f.stops.Add(new KeyValuePair<int, Color>(ratio, ReadColor(r, alpha))); }
            if (f.type == 0x13) r.U16();
            f.color = f.stops.Count > 0 ? f.stops[f.stops.Count / 2].Value : Color.Gray;
        }
        else if (f.type >= 0x40 && f.type <= 0x43) { f.bmp = r.U16(); f.m = ReadMatrix(r); }
        return f;
    }

    static int ReadStyleArrays(BitReader r, Group g, int shapeType)
    {
        int nf = r.U8(); if (nf == 0xFF && shapeType >= 2) nf = r.U16();
        for (int i = 0; i < nf; i++) g.fills.Add(ReadFill(r, shapeType));
        int nl = r.U8(); if (nl == 0xFF && shapeType >= 2) nl = r.U16();
        for (int i = 0; i < nl; i++)
        {
            int w = r.U16();
            if (shapeType == 4)
            {
                int flags = r.U16();
                if (((flags >> 4) & 3) == 2) r.U16();   // miter limit
                Color c;
                if ((flags & 8) != 0) { Fill f = ReadFill(r, shapeType); c = f.color; }
                else c = ReadColor(r, true);
                g.lines.Add(new KeyValuePair<int, Color>(w, c));
            }
            else g.lines.Add(new KeyValuePair<int, Color>(w, ReadColor(r, shapeType >= 3)));
        }
        r.Align();
        return 0;
    }

    static Shape ParseShape(byte[] d, int shapeType)
    {
        var r = new BitReader(d, 2);
        var s = new Shape();
        int nb = (int)r.UB(5); s.minx = r.SB(nb); s.maxx = r.SB(nb); s.miny = r.SB(nb); s.maxy = r.SB(nb); r.Align();
        if (shapeType == 4) { int nb2 = (int)r.UB(5); r.SB(nb2); r.SB(nb2); r.SB(nb2); r.SB(nb2); r.Align(); r.U8(); }
        var g = new Group(); s.groups.Add(g);
        ReadStyleArrays(r, g, shapeType);
        int fillBits = (int)r.UB(4), lineBits = (int)r.UB(4);
        int x = 0, y = 0, f0 = 0, f1 = 0, ln = 0;
        while (true)
        {
            if (r.UB(1) == 1)
            {
                var e = new Edge { x0 = x, y0 = y, f0 = f0, f1 = f1, line = ln };
                if (r.UB(1) == 1)   // straight
                {
                    int n = (int)r.UB(4) + 2;
                    int dx = 0, dy = 0;
                    if (r.UB(1) == 1) { dx = r.SB(n); dy = r.SB(n); }
                    else if (r.UB(1) == 1) dy = r.SB(n); else dx = r.SB(n);
                    x += dx; y += dy; e.x1 = x; e.y1 = y;
                }
                else
                {
                    int n = (int)r.UB(4) + 2;
                    int cdx = r.SB(n), cdy = r.SB(n), adx = r.SB(n), ady = r.SB(n);
                    e.cx = x + cdx; e.cy = y + cdy; x = e.cx + adx; y = e.cy + ady; e.x1 = x; e.y1 = y; e.curve = true;
                }
                g.edges.Add(e);
            }
            else
            {
                uint flags = r.UB(5);
                if (flags == 0) break;
                if ((flags & 1) != 0) { int n = (int)r.UB(5); x = r.SB(n); y = r.SB(n); }
                if ((flags & 2) != 0) f0 = (int)r.UB(fillBits);
                if ((flags & 4) != 0) f1 = (int)r.UB(fillBits);
                if ((flags & 8) != 0) ln = (int)r.UB(lineBits);
                if ((flags & 16) != 0)
                {
                    g = new Group(); s.groups.Add(g);
                    ReadStyleArrays(r, g, shapeType);
                    fillBits = (int)r.UB(4); lineBits = (int)r.UB(4);
                    f0 = f1 = ln = 0;
                }
            }
        }
        return s;
    }

    static void Quad(List<PointF> pts, float x0, float y0, float cx, float cy, float x1, float y1)
    {
        for (int i = 1; i <= 8; i++)
        {
            float t = i / 8f, u = 1 - t;
            pts.Add(new PointF(u * u * x0 + 2 * u * t * cx + t * t * x1, u * u * y0 + 2 * u * t * cy + t * t * y1));
        }
    }

    static Color ApplyCx(Color c, CX cx)
    {
        if (cx == null) return c;
        Func<int, float, int, int> f = (v, m, a) => Math.Max(0, Math.Min(255, (int)(v * m + a)));
        return Color.FromArgb(f(c.A, cx.am, cx.aa), f(c.R, cx.rm, cx.ra), f(c.G, cx.gm, cx.ga), f(c.B, cx.bm, cx.ba));
    }

    static void DrawShape(Graphics g, Shape s, CX cx)
    {
        foreach (var grp in s.groups)
        {
            for (int fi = 1; fi <= grp.fills.Count; fi++)
            {
                var segs = new List<Edge>();
                foreach (var e in grp.edges)
                {
                    if (e.f1 == fi) segs.Add(e);
                    if (e.f0 == fi) segs.Add(new Edge { x0 = e.x1, y0 = e.y1, x1 = e.x0, y1 = e.y0, cx = e.cx, cy = e.cy, curve = e.curve });
                }
                if (segs.Count == 0) continue;
                var byStart = new Dictionary<long, List<int>>();
                for (int i = 0; i < segs.Count; i++) { long k = ((long)segs[i].x0 << 32) ^ (uint)segs[i].y0; if (!byStart.ContainsKey(k)) byStart[k] = new List<int>(); byStart[k].Add(i); }
                var used = new bool[segs.Count];
                var path = new GraphicsPath(FillMode.Winding);
                for (int i = 0; i < segs.Count; i++)
                {
                    if (used[i]) continue;
                    var pts = new List<PointF>(); int cur = i; used[i] = true;
                    pts.Add(new PointF(segs[i].x0, segs[i].y0));
                    while (true)
                    {
                        var e = segs[cur];
                        if (e.curve) Quad(pts, e.x0, e.y0, e.cx, e.cy, e.x1, e.y1); else pts.Add(new PointF(e.x1, e.y1));
                        long k = ((long)e.x1 << 32) ^ (uint)e.y1;
                        int next = -1;
                        if (byStart.ContainsKey(k)) foreach (int j in byStart[k]) if (!used[j]) { next = j; break; }
                        if (next < 0) break;
                        used[next] = true; cur = next;
                    }
                    if (pts.Count >= 3) { path.StartFigure(); path.AddPolygon(pts.ToArray()); }
                }
                var fill = grp.fills[fi - 1];
                Brush br = null;
                if (fill.type == 0) br = new SolidBrush(ApplyCx(fill.color, cx));
                else if (fill.type >= 0x40 && images.ContainsKey(fill.bmp))
                {
                    var img = images[fill.bmp];
                    var ia = new ImageAttributes();
                    if (cx != null) ia.SetColorMatrix(new ColorMatrix(new float[][] {
                        new float[] { cx.rm, 0, 0, 0, 0 }, new float[] { 0, cx.gm, 0, 0, 0 }, new float[] { 0, 0, cx.bm, 0, 0 },
                        new float[] { 0, 0, 0, cx.am, 0 }, new float[] { cx.ra / 255f, cx.ga / 255f, cx.ba / 255f, cx.aa / 255f, 1 } }));
                    var tb = new TextureBrush(img, new Rectangle(0, 0, img.Width, img.Height), ia);
                    tb.WrapMode = (fill.type == 0x40 || fill.type == 0x42) ? WrapMode.Tile : WrapMode.Clamp;
                    // bitmap space is 1 px = 20 twips scaled by the fill matrix
                    var m = fill.m.Clone(); m.Scale(1f / 1f, 1f / 1f);
                    var full = new Matrix(1, 0, 0, 1, 0, 0);
                    tb.Transform = fill.m;
                    br = tb;
                }
                else if (fill.type == 0x10 && fill.stops.Count >= 2)
                {
                    var p0 = new PointF[] { new PointF(-16384, 0), new PointF(16384, 0) };
                    fill.m.TransformPoints(p0);
                    var lg = new LinearGradientBrush(p0[0], p0[1], ApplyCx(fill.stops[0].Value, cx), ApplyCx(fill.stops[fill.stops.Count - 1].Value, cx));
                    var cb = new ColorBlend(fill.stops.Count);
                    for (int k = 0; k < fill.stops.Count; k++) { cb.Positions[k] = fill.stops[k].Key / 255f; cb.Colors[k] = ApplyCx(fill.stops[k].Value, cx); }
                    cb.Positions[0] = 0; cb.Positions[cb.Positions.Length - 1] = 1;
                    try { lg.InterpolationColors = cb; } catch { }
                    br = lg;
                }
                else br = new SolidBrush(ApplyCx(fill.color, cx));
                g.FillPath(br, path);
                br.Dispose(); path.Dispose();
            }
            // strokes
            for (int li = 1; li <= grp.lines.Count; li++)
            {
                var pen = new Pen(ApplyCx(grp.lines[li - 1].Value, cx), Math.Max(20, grp.lines[li - 1].Key));
                pen.StartCap = pen.EndCap = LineCap.Round; pen.LineJoin = LineJoin.Round;
                foreach (var e in grp.edges)
                {
                    if (e.line != li) continue;
                    if (e.curve) { var pts = new List<PointF> { new PointF(e.x0, e.y0) }; Quad(pts, e.x0, e.y0, e.cx, e.cy, e.x1, e.y1); g.DrawLines(pen, pts.ToArray()); }
                    else g.DrawLine(pen, e.x0, e.y0, e.x1, e.y1);
                }
                pen.Dispose();
            }
        }
    }

    static List<Place> Frame1(byte[] d)
    {
        var list = new List<Place>();
        int pos = 4;
        var disp = new SortedDictionary<int, Place>();
        while (pos < d.Length)
        {
            int h = d[pos] | (d[pos + 1] << 8); pos += 2;
            int code = h >> 6, len = h & 63;
            if (len == 63) { len = BitConverter.ToInt32(d, pos); pos += 4; }
            int end = pos + len;
            if (code == 1 || code == 0) break;   // ShowFrame: the first frame is complete
            if (code == 26 || code == 70)
            {
                int flags = d[pos]; int p = pos + 1;
                if (code == 70) p++;
                int depth = d[p] | (d[p + 1] << 8); p += 2;
                var pl = new Place { depth = depth };
                if ((flags & 2) != 0) { pl.chr = d[p] | (d[p + 1] << 8); p += 2; }
                var r = new BitReader(d, p);
                if ((flags & 4) != 0) { pl.m = ReadMatrix(r); }
                if ((flags & 8) != 0)
                {
                    r.Align();
                    bool hasAdd = r.UB(1) == 1, hasMul = r.UB(1) == 1; int nb = (int)r.UB(4);
                    var c = new CX();
                    if (hasMul) { c.rm = r.SB(nb) / 256f; c.gm = r.SB(nb) / 256f; c.bm = r.SB(nb) / 256f; c.am = r.SB(nb) / 256f; }
                    if (hasAdd) { c.ra = r.SB(nb); c.ga = r.SB(nb); c.ba = r.SB(nb); c.aa = r.SB(nb); }
                    pl.cx = new float[] { c.rm, c.gm, c.bm, c.am, c.ra, c.ga, c.ba, c.aa };
                }
                if (pl.chr >= 0) disp[depth] = pl;
            }
            pos = end;
        }
        list.AddRange(disp.Values);
        return list;
    }

    static void Walk(Graphics g, int chr, Matrix acc, CX cx, int depth, Action<Shape, Matrix> onShape)
    {
        if (depth > 8) return;
        if (shapes.ContainsKey(chr))
        {
            var st = g != null ? g.Transform : null;
            onShape(shapes[chr], acc);
            return;
        }
        if (!sprites.ContainsKey(chr)) return;
        foreach (var pl in Frame1(sprites[chr]))
        {
            var m = acc.Clone(); m.Multiply(pl.m, MatrixOrder.Prepend);
            CX c2 = cx;
            if (pl.cx != null)
            {
                c2 = new CX { rm = pl.cx[0], gm = pl.cx[1], bm = pl.cx[2], am = pl.cx[3], ra = (int)pl.cx[4], ga = (int)pl.cx[5], ba = (int)pl.cx[6], aa = (int)pl.cx[7] };
                if (cx != null) { c2.am *= cx.am; }
            }
            Walk(g, pl.chr, m, c2, depth + 1, onShape);
        }
    }

    internal static void Main(string[] a)
    {
        byte[] sw = File.ReadAllBytes(a[0]);
        string atlas = a[1], outDir = a[2]; int size = int.Parse(a[3]);
        Directory.CreateDirectory(outDir);
        // body of a CWS file
        if (sw[0] == 'C')
        {
            using (var ms = new MemoryStream(sw, 10, sw.Length - 10))   // skip 8 byte header + 2 zlib header bytes
            using (var ds = new System.IO.Compression.DeflateStream(ms, System.IO.Compression.CompressionMode.Decompress))
            using (var o = new MemoryStream()) { ds.CopyTo(o); body = o.ToArray(); }
        }
        else { body = new byte[sw.Length - 8]; Array.Copy(sw, 8, body, 0, body.Length); }   // GFX / FWS are stored uncompressed
        int nb = body[0] >> 3; int pos = (5 + nb * 4 + 7) / 8 + 4;
        while (pos + 2 <= body.Length)
        {
            int h = body[pos] | (body[pos + 1] << 8); pos += 2;
            int code = h >> 6, len = h & 63;
            if (len == 63) { len = BitConverter.ToInt32(body, pos); pos += 4; }
            byte[] d = new byte[len]; Array.Copy(body, pos, d, 0, len); pos += len;
            int id = len >= 2 ? d[0] | (d[1] << 8) : 0;
            try
            {
                if (code == 2) shapes[id] = ParseShape(d, 1);
                else if (code == 22) shapes[id] = ParseShape(d, 2);
                else if (code == 32) shapes[id] = ParseShape(d, 3);
                else if (code == 83) shapes[id] = ParseShape(d, 4);
                else if (code == 39) sprites[id] = d;
                else if (code == 1001)
                {
                    int nl = d[9]; string name = System.Text.Encoding.ASCII.GetString(d, 10, nl);   // e.g. pda_I1.tga
                    string png = Path.Combine(atlas, "UI_PDA_" + Path.GetFileNameWithoutExtension(name) + ".png");
                    if (name.StartsWith("pda_I140")) png = Path.Combine(atlas, "L.png"); else if (name.StartsWith("pda_I142")) png = Path.Combine(atlas, "R.png");
                    if (File.Exists(png)) images[id] = new Bitmap(png); else Console.WriteLine("missing " + png);
                }
                if (code == 0) break;
            }
            catch (Exception ex) { Console.WriteLine("tag " + code + " id " + id + ": " + ex.Message); }
            if (code == 0) break;
        }
        Console.WriteLine("shapes " + shapes.Count + " sprites " + sprites.Count + " images " + images.Count);

        var atlasIcons = new List<Bitmap>();
        // optional first name argument "--anchor=ox,oy,scale": every sprite is drawn with its own origin (the pin tip / badge centre)
        // at (ox, oy) of the cell and one common scale (pixels per twip at a 128 px cell), so all icons keep their real proportions
        float anchorX = -1, anchorY = 0, anchorScale = 0; int first = 4;
        if (a.Length > 4 && a[4].StartsWith("--anchor="))
        {
            var pr = a[4].Substring(9).Split(',');
            anchorX = float.Parse(pr[0], System.Globalization.CultureInfo.InvariantCulture); anchorY = float.Parse(pr[1], System.Globalization.CultureInfo.InvariantCulture);
            anchorScale = float.Parse(pr[2], System.Globalization.CultureInfo.InvariantCulture); first = 5;
        }
        var sheet = new Bitmap(size * 8, size * ((a.Length - first + 7) / 8), PixelFormat.Format32bppArgb);
        var sg = Graphics.FromImage(sheet); sg.Clear(Color.FromArgb(255, 60, 70, 80));
        int idx = 0;
        for (int i = first; i < a.Length; i++)
        {
            var kv = a[i].Split('='); string name = kv[0]; int cid = int.Parse(kv[1]);
            // pass 1: bounds
            float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
            Walk(null, cid, new Matrix(), null, 0, (s, m) =>
            {
                var pts = new PointF[] { new PointF(s.minx, s.miny), new PointF(s.maxx, s.miny), new PointF(s.minx, s.maxy), new PointF(s.maxx, s.maxy) };
                m.TransformPoints(pts);
                foreach (var p in pts) { minx = Math.Min(minx, p.X); miny = Math.Min(miny, p.Y); maxx = Math.Max(maxx, p.X); maxy = Math.Max(maxy, p.Y); }
            });
            if (minx > maxx) { Console.WriteLine(name + ": empty"); continue; }
            float w = maxx - minx, h2 = maxy - miny, ss = 4;
            float scale = (size * ss * 0.94f) / Math.Max(w, h2);
            if (anchorX >= 0) scale = anchorScale * (size / 128f) * ss;
            var big = new Bitmap((int)(size * ss), (int)(size * ss), PixelFormat.Format32bppArgb);
            using (var g = Graphics.FromImage(big))
            {
                g.SmoothingMode = SmoothingMode.AntiAlias; g.InterpolationMode = InterpolationMode.HighQualityBicubic; g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                var basem = new Matrix();
                if (anchorX >= 0) { basem.Translate(size * ss * anchorX, size * ss * anchorY); basem.Scale(scale, scale); }
                else { basem.Translate(size * ss * 0.5f, size * ss * 0.5f); basem.Scale(scale, scale); basem.Translate(-(minx + maxx) * 0.5f, -(miny + maxy) * 0.5f); }
                Walk(g, cid, new Matrix(), null, 0, (s, m) =>
                {
                    var full = basem.Clone(); full.Multiply(m, MatrixOrder.Prepend);
                    g.Transform = full;
                    DrawShape(g, s, null);
                });
            }
            var icon = new Bitmap(size, size, PixelFormat.Format32bppArgb);
            using (var g = Graphics.FromImage(icon)) { g.InterpolationMode = InterpolationMode.HighQualityBicubic; g.DrawImage(big, 0, 0, size, size); }
            icon.Save(Path.Combine(outDir, name + ".png"), ImageFormat.Png);
            sg.DrawImage(icon, (idx % 8) * size, (idx / 8) * size); idx++; atlasIcons.Add(icon);
            Console.WriteLine(name + " bounds " + minx.ToString(System.Globalization.CultureInfo.InvariantCulture) + " " + miny.ToString(System.Globalization.CultureInfo.InvariantCulture) + " " + maxx.ToString(System.Globalization.CultureInfo.InvariantCulture) + " " + maxy.ToString(System.Globalization.CultureInfo.InvariantCulture));
        }
        sheet.Save(Path.Combine(outDir, "_sheet.png"), ImageFormat.Png);
        // raw atlas for the mod: "ICON", cols, cell, count, then rows of BGRA pixels (cols*cell wide, rows*cell high)
        {
            int cols = 8, rows = (atlasIcons.Count + cols - 1) / cols, W = cols * size, H = rows * size;
            var at = new Bitmap(W, H, PixelFormat.Format32bppArgb);
            using (var g = Graphics.FromImage(at)) { g.Clear(Color.Transparent); for (int i = 0; i < atlasIcons.Count; i++) g.DrawImage(atlasIcons[i], (i % cols) * size, (i / cols) * size, size, size); }
            var bd = at.LockBits(new Rectangle(0, 0, W, H), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            byte[] px = new byte[W * H * 4];
            for (int y = 0; y < H; y++) System.Runtime.InteropServices.Marshal.Copy(bd.Scan0 + y * bd.Stride, px, y * W * 4, W * 4);
            at.UnlockBits(bd);
            using (var fs = File.Create(Path.Combine(outDir, "icons.bgra")))
            {
                fs.Write(System.Text.Encoding.ASCII.GetBytes("ICON"), 0, 4);
                fs.Write(BitConverter.GetBytes(cols), 0, 4); fs.Write(BitConverter.GetBytes(size), 0, 4); fs.Write(BitConverter.GetBytes(atlasIcons.Count), 0, 4);
                fs.Write(px, 0, px.Length);
            }
        }
    }
}
