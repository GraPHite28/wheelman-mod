using System;
using System.Collections.Generic;
using System.IO;
using UELib;

// Builds the three files the mod's map features need (MapData\citymap.dxt1, icons.bgra, roads.bin) from the player's OWN copy of the
// game. Nothing of the game is shipped with the mod: the city map picture, the marker icons and the road mask come from the game's
// UI_PDA package (the in-game PDA map) and are generated here.
//
//   MapDataBuilder <Wheelman.exe | game folder | UI_PDA.xxx> <output folder> [--keep]
//
// Steps: decompress UI_PDA.xxx -> list its exports (UELib) -> extract the map textures and the atlas pictures (TexDump) -> extract
// the Scaleform movie "pda" -> combine the two map halves into citymap.dxt1 -> derive roads.bin from the map picture (RoadMask)
// -> draw the marker sprites of the movie into icons.bgra (SwfIcons).
class MapDataBuilder
{
    static readonly string[] IconSprites =
    {
        // the order of this list is the order of the atlas (see enum Sprite in MiniMap.cpp)
        "hot_potato=398", "rampage=400", "romanian=405", "contracts=407", "race=409", "fugitive=411", "made_to_order=416", "taxi=420",
        "los_lantos=425", "chulos_canalles=430", "neutral=435", "rank_c=437", "rank_b=478", "rank_a=481", "rank_s=484", "police=442",
        "finish=448", "checkpoint=453", "mission_world=456", "boss=465", "ak47=487", "colt=490", "grenade=493", "minimi=496",
        "mp5=499", "shotgun=502", "uzi=505", "beretta=508", "garage=511", "map_marker=300", "mission=308", "police_influence=305"
    };

    static string FindPdaPackage(string input)
    {
        if (File.Exists(input) && Path.GetFileName(input).StartsWith("UI_PDA", StringComparison.OrdinalIgnoreCase)) return input;
        string dir = File.Exists(input) ? Path.GetDirectoryName(input) : input;
        for (int i = 0; i < 6 && !string.IsNullOrEmpty(dir); ++i, dir = Path.GetDirectoryName(dir))
        {
            string[] candidates =
            {
                Path.Combine(dir, "PC", "WheelmanGame", "CookedPC", "UI_PDA.xxx"),
                Path.Combine(dir, "WheelmanGame", "CookedPC", "UI_PDA.xxx"),
                Path.Combine(dir, "CookedPC", "UI_PDA.xxx"),
                Path.Combine(dir, "UI_PDA.xxx"),
            };
            foreach (string c in candidates) if (File.Exists(c)) return c;
        }
        return null;
    }

    static int Main(string[] args)
    {
        if (args.Length < 2)
        {
            Console.WriteLine("usage: MapDataBuilder <Wheelman.exe | game folder | UI_PDA.xxx> <output folder> [--keep]");
            return 2;
        }
        bool keep = Array.IndexOf(args, "--keep") >= 0;
        string pda = FindPdaPackage(args[0]);
        if (pda == null) { Console.WriteLine("ERROR: UI_PDA.xxx not found near '" + args[0] + "' (expected ...\\PC\\WheelmanGame\\CookedPC\\UI_PDA.xxx)"); return 3; }
        string outDir = Path.GetFullPath(args[1]);
        string work = Path.Combine(Path.GetTempPath(), "WheelmanMapData_" + Guid.NewGuid().ToString("N").Substring(0, 8));
        string png = Path.Combine(work, "png"), raw = Path.Combine(work, "raw"), icons = Path.Combine(work, "icons");
        Directory.CreateDirectory(png); Directory.CreateDirectory(raw); Directory.CreateDirectory(outDir);
        try
        {
            Console.WriteLine("[1/6] decompressing " + pda);
            string upk = Path.Combine(work, "UI_PDA.upk");
            Decomp.Main(new[] { pda, upk });

            Console.WriteLine("[2/6] reading the package");
            var package = UnrealPackage.DeserializePackage(upk, FileAccess.Read);
            byte[] data = File.ReadAllBytes(upk);
            var textures = new List<KeyValuePair<string, int[]>>();
            int swfOffset = -1, swfSize = 0;
            for (int i = 0; i < package.Exports.Count; i++)
            {
                var e = package.Exports[i];
                string cls = e.ClassIndex.Index == 0 ? "Class" : (e.ClassIndex.Index < 0 ? package.Imports[-e.ClassIndex.Index - 1].ObjectName.ToString() : package.Exports[e.ClassIndex.Index - 1].ObjectName.ToString());
                string name = e.ObjectName.ToString();
                if (cls == "Texture2D" && name.StartsWith("pda_")) textures.Add(new KeyValuePair<string, int[]>(name, new[] { e.SerialOffset, e.SerialSize }));
                if (cls == "MwyUIMovieData" && name == "pda") { swfOffset = e.SerialOffset; swfSize = e.SerialSize; }
            }
            if (textures.Count == 0 || swfOffset < 0) { Console.WriteLine("ERROR: the package does not look like UI_PDA (no pda_* textures / no pda movie)"); return 4; }

            Console.WriteLine("[3/6] extracting " + textures.Count + " textures");
            foreach (var t in textures)
            {
                string file = t.Key == "pda_I140" ? "L.png" : (t.Key == "pda_I142" ? "R.png" : "UI_PDA_" + t.Key + ".png");
                var a = new List<string> { upk, t.Value[0].ToString(), t.Value[1].ToString(), "0", "0", "0", Path.Combine(png, file) };
                if (t.Key == "pda_I140") a.Add(Path.Combine(raw, "L.raw"));
                if (t.Key == "pda_I142") a.Add(Path.Combine(raw, "R.raw"));
                try { TexDump.Main(a.ToArray()); }
                catch (Exception ex) { Console.WriteLine("  (skipped " + t.Key + ": " + ex.Message + ")"); }
            }

            Console.WriteLine("[4/6] city map");
            byte[] L = File.ReadAllBytes(Path.Combine(raw, "L.raw")), R = File.ReadAllBytes(Path.Combine(raw, "R.raw"));
            if (L.Length != 8388608 || R.Length != 4194304) { Console.WriteLine("ERROR: unexpected map texture sizes " + L.Length + " / " + R.Length); return 5; }
            using (var fs = File.Create(Path.Combine(outDir, "citymap.dxt1")))
            {
                fs.Write(System.Text.Encoding.ASCII.GetBytes("MAP1"), 0, 4);
                fs.Write(BitConverter.GetBytes(6144), 0, 4); fs.Write(BitConverter.GetBytes(4096), 0, 4);
                for (int row = 0; row < 1024; row++) { fs.Write(L, row * 8192, 8192); fs.Write(R, row * 4096, 4096); }
            }

            Console.WriteLine("[5/6] road mask");
            RoadMask.Main(new[] { png, outDir });

            Console.WriteLine("[6/6] marker icons");
            int gfx = -1;
            for (int i = swfOffset; i + 4 < swfOffset + swfSize && i < swfOffset + 200; i++)
                if (data[i] == 'G' && data[i + 1] == 'F' && data[i + 2] == 'X') { gfx = i; break; }
            if (gfx < 0) { Console.WriteLine("ERROR: the Scaleform movie was not found in the pda export"); return 6; }
            string swf = Path.Combine(work, "pda.swf");
            var swfBytes = new byte[swfOffset + swfSize - gfx];
            Array.Copy(data, gfx, swfBytes, 0, swfBytes.Length);
            File.WriteAllBytes(swf, swfBytes);
            var sa = new List<string> { swf, png, icons, "128", "--anchor=0.4,0.75,0.0515" };
            sa.AddRange(IconSprites);
            SwfIcons.Main(sa.ToArray());
            File.Copy(Path.Combine(icons, "icons.bgra"), Path.Combine(outDir, "icons.bgra"), true);

            Console.WriteLine("DONE: " + outDir);
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine("ERROR: " + ex.Message);
            return 1;
        }
        finally
        {
            if (!keep) { try { Directory.Delete(work, true); } catch { } }
            else Console.WriteLine("work files kept in " + work);
        }
    }
}
