# Compilación exacta de ZERODROID v0.8.4.4

## Método A — Docker, recomendado

Requisitos:

- Docker Engine o Docker Desktop.
- Al menos 4 GB libres.
- Conexión para descargar `devkitpro/devkita64:latest` la primera vez.

Linux/macOS:

```bash
chmod +x VERIFY_KIT.sh COMPILE_WITH_DOCKER.sh
./VERIFY_KIT.sh
./COMPILE_WITH_DOCKER.sh
```

Windows PowerShell:

```powershell
.\VERIFY_KIT.ps1
.\COMPILE_WITH_DOCKER.ps1
```

## Método B — comando Docker manual

```bash
docker run --rm \
  -v "$PWD/ZERODROID_v0.8.4.4:/workspace" \
  -w /workspace/switch \
  devkitpro/devkita64:latest \
  bash -lc 'set -euo pipefail; make clean || true; make -j"$(nproc)"'
```

## Método C — GitHub Actions

1. Subir `ZERODROID_v0.8.4.4/` como raíz del repositorio o copiar su contenido a
   la raíz del repositorio.
2. Confirmar que exista `.github/workflows/build-v0.8.4.4.yml`.
3. Abrir Actions → Build ZERODROID v0.8.4.4 → Run workflow.
4. Descargar el artifact `ZERODROID-v0.8.4.4`.

## Output válido

Solo es válido este archivo generado por el Makefile actual:

```text
switch/ZERODROID_v0.8.4.4.nro
```

Debe acompañarse con:

```text
switch/ZERODROID_v0.8.4.4.nro.sha256
build-v0.8.4.4.log
```

## Dependencias ya incluidas

No ejecutar `switch/deps/build-switch.sh` salvo ausencia real de las bibliotecas.
El kit incluye headers y bibliotecas estáticas de libpeer, SRTP, usrsctp y
mbedTLS. También incluye los shaders `.dksh` generados.

## Errores comunes

- `DEVKITPRO` vacío: se está compilando fuera de la imagen devkitPro.
- `switch_rules: No such file`: falta el entorno devkitA64.
- `cannot find -lpeer`: verificar `switch/deps/switch/lib/libpeer.a`.
- shaders faltantes: verificar `switch/romfs/shaders/*.dksh`.
- output con nombre viejo: limpiar build y verificar `APP_VERSION/TARGET`.
