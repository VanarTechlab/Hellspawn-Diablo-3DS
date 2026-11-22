# Hellspawn: Diablo 3DS

Port gratuito de **Diablo** para **Nintendo 3DS** (todos los modelos; más fluido en New 3DS), por **Vanar** ([VanarTechlab](https://www.youtube.com/@VanarTechlab)).
Basado en [DevilutionX](https://github.com/diasurgical/devilutionX) y en [Hellspawn – Diablo DSi](https://github.com/VanarTechlab/Hellspawn-Diablo-DSI).
Web: <https://vanartechlab.pages.dev/hellspawn3ds/>

> **Esta es una versión modificada de DevilutionX.** Ver [MODIFICACIONES.md](MODIFICACIONES.md).
> Proyecto no oficial, sin relación con Blizzard Entertainment.

## Descargar y jugar

La versión lista para jugar está en **[Releases](https://github.com/VanarTechlab/Hellspawn-Diablo-3DS/releases)**
(`.cia` para el menú HOME y `.3dsx` para el Homebrew Launcher). Es gratis y lo será siempre.
**No incluye archivos de Blizzard**: copia tu propio `DIABDAT.MPQ` a `sdmc:/3ds/devilutionx/`.
Requiere una 3DS / 2DS con firmware personalizado; en New 3DS / New 2DS XL va más fluido.

## Qué tiene

- Pantalla de arriba en modo ancho (800×240) solo con el mundo, más cerca que en DevilutionX.
- Pantalla de abajo: panel clásico de Diablo, minimapa, inventario y ventanas a pantalla completa.
  La táctil solo toca botones y objetos: el personaje no se mueve con ella.
- Descripción de objetos arriba, barra de experiencia verde, aviso de subida de nivel, cargas panorámicas.
- Conversión de imagen en el segundo núcleo de la New 3DS (~50 fps en partida).
- Cambios de juego de Hellspawn: misiones nuevas (bosque, guarida del Carnicero con cinemática),
  frases del héroe, oro automático, XP compartida, regla PAZ/PVP.
- Extras: Cinemáticas, Soundtrack, BestiarioDex, idioma Español/English, créditos 3DS.
- Voces al cerrar/abrir la tapa. Multijugador TCP entre consolas con Hellspawn.

## Código

Rama `main` = DevilutionX + los cambios de Hellspawn (DSi y 3DS). Los cambios están marcados con comentarios:

| Marca | Qué es |
|---|---|
| `/* HS3DS-XXX */` y `#if defined(HELLSPAWN) && defined(__3DS__)` | Cambios propios de la 3DS |
| `/* HS-XXX */` y `#ifdef HELLSPAWN` | Cambios de juego compartidos (Hellspawn DSi y Belial 64) |
| `/* NDS-XXX */` y `#ifdef __NDS__` | Cambios de la versión DSi (no se compilan en la 3DS) |

Archivos principales de la 3DS: `Source/engine/dx.cpp` (salida a las dos pantallas, modo ancho, hilo del núcleo 2,
tapa), `Source/engine/events.cpp` (táctil), `Source/engine/render/scrollrt.cpp` (zonas de la pantalla),
`Source/DiabloUI/mainmenu.cpp` (menús y Extras), `CMake/platforms/n3ds.cmake`.

## Compilar (Ubuntu / WSL)

Necesitas [devkitPro](https://devkitpro.org/) con `devkitARM`, `libctru`, `citro3d`, `3ds-sdl`, `bannertool` y `makerom`.

```
cmake -S . -B build-3ds -G Ninja -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/3DS.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-3ds
```

Salen `build-3ds/devilutionx.3dsx` y `build-3ds/devilutionx.cia`.
El arte del menú (Night Stroke) y la cinemática del Carnicero **no** están en este repositorio.

## Licencias

- DevilutionX: **Sustainable Use License 1.0** ([LICENSE.md](LICENSE.md)): uso y reparto **gratis y sin fines comerciales**. Hellspawn mantiene esa licencia.
- Diablo y sus datos son de Blizzard Entertainment y **no** están en este repositorio.

## Créditos

- **Vanar**: port a 3DS.
- **Night Stroke**: assets del menú, logo e icono (no incluidos en el código).
- **DevilutionX Team**: el motor.
- **Blizzard North**: creadores de Diablo.

Las donaciones son voluntarias y no desbloquean nada: <https://vanar-techlab-shop.fourthwall.com>

---

*English:* Free, non-commercial Diablo port (DevilutionX) for Nintendo 3DS (smoother on New 3DS). Bring your own `DIABDAT.MPQ`.
This is a modified version of DevilutionX; see `MODIFICACIONES.md`. Not affiliated with Blizzard.
