Import("env")
import subprocess

def convert(source, target, env):
    print("=== Converting to UF2...")
    print(str(target[0]))
    hex_file = str(target[0])
    subprocess.run([
        "python", "tools/uf2conv.py",
        hex_file,
        "-c",
        "-b", "0x27000",
        "-f", "0xADA52840",
        "-o", "firmware.uf2"
    ])
    print("=== Done.")  

env.AddPostAction("$BUILD_DIR/firmware.hex", convert)
