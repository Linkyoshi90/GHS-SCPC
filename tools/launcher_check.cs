// Self-check for the launcher's ISO reader: every file on the disc image must match the one in game\ (SHA1).
//   C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /nologo /main:LauncherCheck /out:%TEMP%\launcher_check.exe tools\launcher.cs tools\launcher_check.cs
//   %TEMP%\launcher_check.exe <disc.iso> game
using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;

static class LauncherCheck
{
    static int Main(string[] args)
    {
        int bad = 0;
        using (var iso = new Iso(args[0]))
        {
            var files = new List<KeyValuePair<string, Iso.Entry>>();
            iso.Collect(iso.Root, "", files);
            foreach (var f in files)
            {
                string local = Path.Combine(args[1], f.Key);
                string want = File.Exists(local) ? Hash(File.OpenRead(local)) : "missing";
                var sha = SHA1.Create();
                using (var sink = new CryptoStream(Stream.Null, sha, CryptoStreamMode.Write)) iso.CopyFile(f.Value, sink, n => { });
                string got = BitConverter.ToString(sha.Hash).Replace("-", "");
                if (got != want) bad++;
                Console.WriteLine((got == want ? "ok   " : "BAD  ") + f.Key + " " + f.Value.Size);
            }
            Console.WriteLine(files.Count + " files, " + bad + " different");
        }
        return bad;
    }

    static string Hash(Stream s)
    {
        using (s) using (var sha = SHA1.Create()) return BitConverter.ToString(sha.ComputeHash(s)).Replace("-", "");
    }
}
