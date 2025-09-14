cmd_bsp/built-in.a := echo >/dev/null; rm -f bsp/built-in.a; aarch64-linux-gnu-ar cDPrST bsp/built-in.a bsp/drivers/built-in.a bsp/modules/nand/built-in.a bsp/modules/gpu/built-in.a
