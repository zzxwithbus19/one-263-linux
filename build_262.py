import os, shutil, zipfile, subprocess, sys
from pathlib import Path

BASE = Path(r"F:\beif\Desktop\mc_package_build")
STAGE = BASE / "262_stage"
SFX = BASE / "forge_sfx"
OUT = BASE / "forge_out"
ZIP = BASE / "262_embed.zip"

for d in [STAGE, SFX, OUT]:
    if d.exists():
        for p in d.rglob("classes.jsa"):
            try: p.unlink(missing_ok=True)
            except: pass
        shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)

JAVA_RT = Path(r"C:\Users\miniy\AppData\Roaming\.minecraft\runtime\java-runtime-epsilon")
MC_FAN = Path(r"F:\beif\Desktop\mcfan")
LIBS = MC_FAN / "libraries"
VERSION = MC_FAN / "versions" / "262+fo"
ASSETS_SRC = MC_FAN / "assets"

APP = STAGE / "app"
NATIVES = APP / "natives"
RT = STAGE / "runtime"
MODS_STAGE = STAGE / "mods"
for d in [APP, NATIVES, RT, MODS_STAGE]:
    d.mkdir(parents=True)

# --- Copy Java 25 runtime ---
print("Copying Java runtime...")
shutil.copytree(JAVA_RT, RT, dirs_exist_ok=True, ignore=lambda d, files:
    [f for f in files if Path(d).suffix in {'.pack','.pkg'} or Path(d).name == 'src.zip' or f.endswith('.debuginfo')])
rt_size = sum(f.stat().st_size for f in RT.rglob("*"))
print(f"Runtime: {rt_size/1024/1024:.1f} MB")

# --- Copy version jar (stripped) ---
print("Processing 262+fo.jar...")
jar_src = VERSION / "262+fo.jar"
stripped = APP / "mc.jar"
with zipfile.ZipFile(jar_src, 'r') as zin:
    with zipfile.ZipFile(stripped, 'w', zipfile.ZIP_DEFLATED) as zout:
        for item in zin.infolist():
            if item.filename.startswith('META-INF/') and any(
                item.filename.endswith(x) for x in ['.SF','.DSA','.RSA','.EC']
            ): continue
            zout.writestr(item, zin.read(item))

# --- Library jars for 26.2 (modern version, not 1.7.10) ---
lib_jars = [
    "at/yawk/lz4/lz4-java/1.10.1/lz4-java-1.10.1.jar",
    "com/azure/azure-json/1.4.0/azure-json-1.4.0.jar",
    "com/github/oshi/oshi-core/6.9.0/oshi-core-6.9.0.jar",
    "com/google/code/gson/gson/2.14.0/gson-2.14.0.jar",
    "com/google/guava/failureaccess/1.0.3/failureaccess-1.0.3.jar",
    "com/google/guava/guava/33.6.0-jre/guava-33.6.0-jre.jar",
    "com/ibm/icu/icu4j/78.3/icu4j-78.3.jar",
    "com/microsoft/azure/msal4j/1.24.1/msal4j-1.24.1.jar",
    "com/mojang/authlib/9.0.75/authlib-9.0.75.jar",
    "com/mojang/blocklist/1.0.10/blocklist-1.0.10.jar",
    "com/mojang/brigadier/1.3.10/brigadier-1.3.10.jar",
    "com/mojang/datafixerupper/10.0.21/datafixerupper-10.0.21.jar",
    "com/mojang/jtracy/1.0.37/jtracy-1.0.37.jar",
    "com/mojang/jtracy/1.0.37/jtracy-1.0.37-natives-windows.jar",
    "com/mojang/logging/1.7.12/logging-1.7.12.jar",
    "com/mojang/patchy/2.2.10/patchy-2.2.10.jar",
    "com/mojang/text2speech/1.19.12/text2speech-1.19.12.jar",
    "commons-codec/commons-codec/1.22.0/commons-codec-1.22.0.jar",
    "commons-io/commons-io/2.20.0/commons-io-2.20.0.jar",
    "io/netty/netty-buffer/4.2.15.Final/netty-buffer-4.2.15.Final.jar",
    "io/netty/netty-codec-base/4.2.15.Final/netty-codec-base-4.2.15.Final.jar",
    "io/netty/netty-codec-compression/4.2.15.Final/netty-codec-compression-4.2.15.Final.jar",
    "io/netty/netty-codec-http/4.2.15.Final/netty-codec-http-4.2.15.Final.jar",
    "io/netty/netty-common/4.2.15.Final/netty-common-4.2.15.Final.jar",
    "io/netty/netty-handler/4.2.15.Final/netty-handler-4.2.15.Final.jar",
    "io/netty/netty-resolver/4.2.15.Final/netty-resolver-4.2.15.Final.jar",
    "io/netty/netty-transport/4.2.15.Final/netty-transport-4.2.15.Final.jar",
    "io/netty/netty-transport-classes-epoll/4.2.15.Final/netty-transport-classes-epoll-4.2.15.Final.jar",
    "io/netty/netty-transport-classes-kqueue/4.2.15.Final/netty-transport-classes-kqueue-4.2.15.Final.jar",
    "io/netty/netty-transport-native-unix-common/4.2.15.Final/netty-transport-native-unix-common-4.2.15.Final.jar",
    "it/unimi/dsi/fastutil/8.5.18/fastutil-8.5.18.jar",
    "net/fabricmc/fabric-loader/0.19.3/fabric-loader-0.19.3.jar",
    "net/fabricmc/sponge-mixin/0.17.3+mixin.0.8.7/sponge-mixin-0.17.3+mixin.0.8.7.jar",
    "net/java/dev/jna/jna/5.17.0/jna-5.17.0.jar",
    "net/java/dev/jna/jna-platform/5.17.0/jna-platform-5.17.0.jar",
    "net/sf/jopt-simple/jopt-simple/5.0.4/jopt-simple-5.0.4.jar",
    "org/apache/commons/commons-compress/1.28.0/commons-compress-1.28.0.jar",
    "org/apache/commons/commons-lang3/3.20.0/commons-lang3-3.20.0.jar",
    "org/apache/logging/log4j/log4j-api/2.26.0/log4j-api-2.26.0.jar",
    "org/apache/logging/log4j/log4j-core/2.26.0/log4j-core-2.26.0.jar",
    "org/apache/logging/log4j/log4j-slf4j2-impl/2.26.0/log4j-slf4j2-impl-2.26.0.jar",
    "org/jcraft/jorbis/0.0.17/jorbis-0.0.17.jar",
    "org/joml/joml/1.10.8/joml-1.10.8.jar",
    "org/jspecify/jspecify/1.0.0/jspecify-1.0.0.jar",
    # LWJGL 3.4.1 main jars
    "org/lwjgl/lwjgl-freetype/3.4.1/lwjgl-freetype-3.4.1.jar",
    "org/lwjgl/lwjgl-freetype/3.4.1/lwjgl-freetype-3.4.1-natives-windows.jar",
    "org/lwjgl/lwjgl-glfw/3.4.1/lwjgl-glfw-3.4.1.jar",
    "org/lwjgl/lwjgl-glfw/3.4.1/lwjgl-glfw-3.4.1-natives-windows.jar",
    "org/lwjgl/lwjgl-jemalloc/3.4.1/lwjgl-jemalloc-3.4.1.jar",
    "org/lwjgl/lwjgl-jemalloc/3.4.1/lwjgl-jemalloc-3.4.1-natives-windows.jar",
    "org/lwjgl/lwjgl-openal/3.4.1/lwjgl-openal-3.4.1.jar",
    "org/lwjgl/lwjgl-openal/3.4.1/lwjgl-openal-3.4.1-natives-windows.jar",
    "org/lwjgl/lwjgl-opengl/3.4.1/lwjgl-opengl-3.4.1.jar",
    "org/lwjgl/lwjgl-opengl/3.4.1/lwjgl-opengl-3.4.1-natives-windows.jar",
    "org/lwjgl/lwjgl-shaderc/3.4.1/lwjgl-shaderc-3.4.1.jar",
    "org/lwjgl/lwjgl-shaderc/3.4.1/lwjgl-shaderc-3.4.1-natives-windows.jar",
    "org/lwjgl/lwjgl-spvc/3.4.1/lwjgl-spvc-3.4.1.jar",
    "org/lwjgl/lwjgl-spvc/3.4.1/lwjgl-spvc-3.4.1-natives-windows.jar",
    "org/lwjgl/lwjgl-stb/3.4.1/lwjgl-stb-3.4.1.jar",
    "org/lwjgl/lwjgl-stb/3.4.1/lwjgl-stb-3.4.1-natives-windows.jar",
    "org/lwjgl/lwjgl-stb/3.4.1/lwjgl-stb-3.4.1.jar",
    "org/lwjgl/lwjgl-stb/3.4.1/lwjgl-stb-3.4.1-natives-windows.jar",
    "org/lwjgl/lwjgl-tinyfd/3.4.1/lwjgl-tinyfd-3.4.1.jar",
    "org/lwjgl/lwjgl-tinyfd/3.4.1/lwjgl-tinyfd-3.4.1-natives-windows.jar",
    "org/lwjgl/lwjgl-vma/3.4.1/lwjgl-vma-3.4.1.jar",
    "org/lwjgl/lwjgl-vma/3.4.1/lwjgl-vma-3.4.1-natives-windows.jar",
    "org/lwjgl/lwjgl-vulkan/3.4.1/lwjgl-vulkan-3.4.1.jar",
    "org/lwjgl/lwjgl/3.4.1/lwjgl-3.4.1-natives-windows.jar",
    "org/lwjgl/lwjgl/3.4.1/lwjgl-3.4.1-unsafe.jar",
    # Fabric ASM
    "org/ow2/asm/asm/9.10.1/asm-9.10.1.jar",
    "org/ow2/asm/asm-analysis/9.10.1/asm-analysis-9.10.1.jar",
    "org/ow2/asm/asm-commons/9.10.1/asm-commons-9.10.1.jar",
    "org/ow2/asm/asm-tree/9.10.1/asm-tree-9.10.1.jar",
    "org/ow2/asm/asm-util/9.10.1/asm-util-9.10.1.jar",
    "org/slf4j/slf4j-api/2.0.17/slf4j-api-2.0.17.jar",
]

print("Copying library jars...")
for j in lib_jars:
    src = LIBS / j
    if src.exists():
        shutil.copy2(src, APP / src.name)

# --- Copy mods ---
print("Copying mods...")
mods_src = VERSION / "mods"
for mf in mods_src.glob("*.jar"):
    shutil.copy2(mf, MODS_STAGE / mf.name)
mods_count = len(list(MODS_STAGE.glob("*.jar")))
print(f"Mods: {mods_count}")

# --- Copy assets ---
print("Copying assets...")
shutil.copytree(ASSETS_SRC, STAGE / "assets", dirs_exist_ok=True)

# --- Extract LWJGL natives ---
# For modern LWJGL 3, natives are on classpath; but extract DLLs from native jars
print("Extracting natives...")
for nj in (LIBS / "org" / "lwjgl").rglob("*-natives-windows.jar"):
    with zipfile.ZipFile(nj) as zf:
        for e in zf.infolist():
            if e.filename.endswith(".dll") and not e.is_dir():
                dll = NATIVES / Path(e.filename).name
                if not dll.exists():
                    with zf.open(e) as s, open(dll, "wb") as o:
                        print(f"  {dll.name}")
                        shutil.copyfileobj(s, o)
# Also extract from jtracy native jar
for nj in (LIBS / "com" / "mojang" / "jtracy").rglob("*-natives-windows.jar"):
    with zipfile.ZipFile(nj) as zf:
        for e in zf.infolist():
            if e.filename.endswith(".dll") and not e.is_dir():
                dll = NATIVES / Path(e.filename).name
                if not dll.exists():
                    with zf.open(e) as s, open(dll, "wb") as o:
                        print(f"  {dll.name}")
                        shutil.copyfileobj(s, o)

natives_count = len(list(NATIVES.glob("*.dll")))
print(f"Natives: {natives_count} DLLs")

app_size = sum(f.stat().st_size for f in APP.rglob("*"))
as_size = sum(f.stat().st_size for f in (STAGE / "assets").rglob("*"))
mods_size = sum(f.stat().st_size for f in MODS_STAGE.rglob("*"))
print(f"App: {app_size/1024/1024:.1f} MB")
print(f"Assets: {as_size/1024/1024:.1f} MB")
print(f"Mods: {mods_size/1024/1024:.1f} MB")

# --- Create embed ZIP ---
print("Creating embed ZIP...")
with zipfile.ZipFile(ZIP, "w", zipfile.ZIP_STORED) as zf:
    for root, dirs, files in os.walk(STAGE):
        for f in files:
            fp = Path(root) / f
            rel = os.path.relpath(fp, STAGE).replace("\\", "/")
            if not rel.startswith("app/") and not rel.startswith("runtime/") and not rel.startswith("assets/") and not rel.startswith("mods/"):
                continue
            zf.write(fp, rel)
print(f"ZIP: {ZIP.stat().st_size/1024/1024:.1f} MB")

# --- Write C# launcher ---
print("Writing C# launcher...")
(SFX / "Program.cs").write_text('''using System;
using System.IO;
using System.IO.Compression;
using System.Diagnostics;
using System.Reflection;

try
{
    string exePath = Environment.ProcessPath!;
    string playerName = Path.GetFileNameWithoutExtension(exePath);
    string exeDir = Path.GetDirectoryName(exePath)!;
    string dataDir = Path.Combine(exeDir, "_data");
    string modsDir = Path.Combine(exeDir, "mods");
    string runtimeJava = Path.Combine(dataDir, "runtime", "bin", "java.exe");
    string assetsDir = Path.Combine(dataDir, "assets");
    string nativesDir = Path.Combine(dataDir, "app", "natives");
    string logFile = Path.Combine(exeDir, "launcher.log");

    using (var log = new StreamWriter(logFile, false))
    {
        log.WriteLine("Start");
        log.WriteLine("exePath=" + exePath);
        log.WriteLine("playerName=" + playerName);

        if (!File.Exists(runtimeJava))
        {
            log.WriteLine("Extracting...");
            Console.WriteLine("Extracting game files...");
            Directory.CreateDirectory(dataDir);
            using var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("game.zip");
            if (stream == null) throw new Exception("game.zip not found in resources!");
            string zipPath = Path.Combine(dataDir, "data.zip");
            using (var file = File.Create(zipPath)) stream.CopyTo(file);
            log.WriteLine("ZIP saved, size=" + new FileInfo(zipPath).Length);
            ZipFile.ExtractToDirectory(zipPath, dataDir);
            File.Delete(zipPath);

            // Pre-install mods: copy from _data/mods/ to exeDir/mods/
            string embeddedMods = Path.Combine(dataDir, "mods");
            if (Directory.Exists(embeddedMods))
            {
                Directory.CreateDirectory(modsDir);
                foreach (var mf in Directory.GetFiles(embeddedMods, "*.jar"))
                {
                    string dest = Path.Combine(modsDir, Path.GetFileName(mf));
                    log.WriteLine("Installing mod: " + Path.GetFileName(mf));
                    File.Copy(mf, dest);
                }
            }
            log.WriteLine("Extraction done");
        }

        string cp = string.Join(";", Directory.GetFiles(Path.Combine(dataDir, "app"), "*.jar"));
        Directory.CreateDirectory(modsDir);

        // For LWJGL 3, set native library path for Java runtime libs
        string javaHome = Path.Combine(dataDir, "runtime");
        string jreLib = Path.Combine(javaHome, "lib");

        string jvmArgs = string.Format(
            "-Xms2G -Xmx4G -XX:+UseCompactObjectHeaders -XX:+AlwaysPreTouch -XX:+UseStringDeduplication -XX:+UseZGC --sun-misc-unsafe-memory-access=allow --enable-native-access=ALL-UNNAMED -Djava.library.path=\"{0}\" -Djna.tmpdir=\"{1}\" -Dorg.lwjgl.system.SharedLibraryExtractPath=\"{1}\" -Dio.netty.native.workdir=\"{1}\" -Dfabric.development=false \"-DFabricMcEmu= net.minecraft.client.main.Main \" -cp \"{2}\" net.fabricmc.loader.impl.launch.knot.KnotClient --username {3} --version 26.2 --gameDir \"{4}\" --assetsDir \"{5}\" --assetIndex 32 --uuid 00000000-0000-0000-0000-000000000000 --accessToken 0 --clientId 00000000-0000-0000-0000-000000000000 --xuid 0000000000000000 --userType mojang --versionType release",
            nativesDir, Path.Combine(nativesDir, "lwjgl"), cp, playerName, exeDir, assetsDir);

        log.WriteLine("Running: " + runtimeJava);
        log.WriteLine("Args: " + jvmArgs);

        Console.WriteLine("Starting Minecraft...");
        var proc = Process.Start(new ProcessStartInfo
        {
            FileName = runtimeJava,
            Arguments = jvmArgs,
            WorkingDirectory = exeDir,
            UseShellExecute = false
        });
        log.WriteLine("PID=" + (proc?.Id ?? -1));
        proc?.WaitForExit();
        log.WriteLine("Exit=" + (proc?.ExitCode ?? -1));
    }
}
catch (Exception ex)
{
    string errLog = Path.Combine(Path.GetDirectoryName(Environment.ProcessPath!)!, "launcher_error.log");
    File.WriteAllText(errLog, ex.ToString());
    Console.WriteLine("ERROR: " + ex.ToString());
    Console.WriteLine("Press any key...");
    Console.ReadKey();
}
''')

# --- Write .csproj ---
print("Writing .csproj...")
(SFX / "Bootstrap.csproj").write_text(f'''<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net10.0</TargetFramework>
    <RuntimeIdentifier>win-x64</RuntimeIdentifier>
    <SelfContained>true</SelfContained>
    <PublishSingleFile>true</PublishSingleFile>
    <IncludeNativeLibrariesForSelfExtract>true</IncludeNativeLibrariesForSelfExtract>
    <DebugType>none</DebugType>
  </PropertyGroup>
  <ItemGroup>
    <EmbeddedResource Include="{ZIP}" LogicalName="game.zip" />
  </ItemGroup>
</Project>
''')

# --- Build ---
env = {**os.environ, "TEMP": str(BASE / "temp"), "TMP": str(BASE / "temp")}
os.makedirs(env["TEMP"], exist_ok=True)

print("dotnet restore...")
subprocess.run(["dotnet", "restore", str(SFX / "Bootstrap.csproj")], capture_output=True, timeout=120, env=env)

print("dotnet publish...")
r = subprocess.run(
    ["dotnet", "publish", str(SFX / "Bootstrap.csproj"), "-c", "Release", "-o", str(OUT)],
    capture_output=True, text=True, timeout=600, env=env
)
print(f"RC={r.returncode}")
if r.returncode != 0:
    print("STDERR:", r.stderr[:2000])
    sys.exit(1)

# --- Copy result ---
dst_dir = Path(r"F:\beif\Desktop\MC") / "262+fo"
dst_dir.mkdir(parents=True, exist_ok=True)
for exe in OUT.rglob("*.exe"):
    sz = exe.stat().st_size / 1024 / 1024
    print(f"EXE: {exe} ({sz:.1f} MB)")
    dst = dst_dir / "262+fo.exe"
    shutil.copy2(exe, dst)
    print(f"Copied: {dst}")

print("=== DONE ===")
