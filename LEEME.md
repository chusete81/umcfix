# Investigación del fallo de paleta VGA y temporización PCI en una PCChips M915i

## 1. Resumen ejecutivo

Esta investigación partió de un fallo aparentemente gráfico: varios juegos DOS mostraban colores incorrectos cuando actualizaban dinámicamente la paleta VGA. El problema aparecía con una **S3 Trio64V2/DX PCI** y también con una **S3 ViRGE PCI**, en una placa **PCChips M915i** con chipset **UMC UM8881F + UM8886A**, usando un **Intel 486 DX4-100**.

Las primeras pruebas demostraron que el problema no era una simple corrupción analógica del color ni un defecto aislado de una tarjeta VGA. El patrón de errores reveló que algunos accesos de E/S a los puertos del DAC VGA se perdían, repetían o se trataban incorrectamente, desincronizando el estado interno del DAC y provocando largas secuencias de colores desplazados.

La investigación fue progresando desde el síntoma gráfico hacia el subsistema de E/S de la placa base. El hallazgo clave fue que al desactivar TURBO el problema mejoraba drásticamente, pero con una fuerte penalización general de rendimiento. Posteriormente se comprobó que TURBO OFF apenas reducía el rendimiento aritmético interno de la CPU, pero sí ralentizaba mucho los accesos externos, especialmente E/S y escrituras a RAM. Esto apuntó a temporización, wait states o handshake del bus, no al reloj interno del procesador.

Tras descartar varios bits del UM8881F relacionados tentativamente con CPU-to-PCI Post Write y Burst Write, se probó de forma controlada el campo `A4[1:0]` del **UM8886A**. Cambiarlo de `01b` a `00b` produjo un resultado inequívoco:

- La utilidad diagnóstica `VGAPAL4` creada adhoc pasó de miles de errores a **cero errores en todas las pruebas**.
- Los juegos dejaron de mostrar corrupción de paleta.
- El logo de Windows 95 durante el arranque dejó de presentar problemas relacionados con el color.
- El adaptador Ethernet **RTL8029**, que antes perdía paquetes y hacía inviables transferencias FTP en Windows, comenzó a funcionar correctamente.
- CPU y RAM mantuvieron prácticamente el mismo rendimiento que con TURBO ON.
- La penalización observable en Doom fue moderada: el `timedemo` pasó de **1891** a **2009 realtics**, aproximadamente un **6,24 % más de tiempo** o un **5,87 % menos de rendimiento efectivo**.

Se concluyó que la configuración original `A4[1:0]=01b` dejaba el subsistema PCI/E/S en una temporización demasiado agresiva para esta placa. El estado `00b` actúa como una configuración estable y constituye una solución práctica y reproducible.

Como solución definitiva se creó **UMCFIX**, una pequeña utilidad para ejecutar desde `AUTOEXEC.BAT`, diseñada exclusivamente para detectar el hardware esperado y aplicar de forma segura `A4[1:0]=00b`.

---

## 2. Sistema investigado

### Placa base

- **Modelo:** PCChips M915i
- **Chipset:** UMC UM8881F + UM8886A
- La placa es conocida por usar una implementación de “fake cache”, aunque la (no-)caché no resultó ser la causa de este problema.

### CPU

- **Intel 486 DX4-100**

### Tarjetas gráficas probadas

- **S3 Trio64V2/DX PCI**, 1 MB de VRAM
- **S3 ViRGE PCI**, 2 MB de VRAM

Ambas afectadas (también por el problema de 'pedestal bit', solucionado aparte).

### Sistema operativo

- Modo MSDOS de Windows 95 OSR2.1

### Otros dispositivos relevantes

- Tarjeta de red PCI basada en **Realtek RTL8029**

---

## 3. Síntomas iniciales

El problema se observaba especialmente en software que actualizaba con frecuencia la paleta VGA.

Juegos afectados:

- Doom
- Duke Nukem 3D
- Prince of Persia (1 y 2)
- The Lion King
- Mortal Kombat II

También se observaron síntomas durante:

- logo de arranque y apagado de Windows 95;
- fades;
- flashes;
- efectos que modificaban rápidamente el DAC VGA.

El fallo no estaba necesariamente ligado a modos VESA; principalmente se reproducía en **modo VGA 13h**, por lo que el problema debía encontrarse en una ruta suficientemente básica como para afectar al DAC VGA estándar.

Una característica importante era que los colores incorrectos podían permanecer hasta la siguiente actualización de la paleta, lo que sugería que el DAC estaba recibiendo datos incorrectos y conservándolos, en lugar de tratarse de ruido analógico momentáneo.

---

## 4. Hipótesis iniciales consideradas

Durante la investigación se contemplaron, entre otras, las siguientes posibilidades:

1. Defecto de la tarjeta S3 Trio64V2/DX.
2. Problema del RAMDAC integrado.
3. Problema de VRAM.
4. Corrupción analógica en la salida VGA.
5. Interacción específica S3-chipset.
6. Problema de caché o memoria.
7. Temporización incorrecta de los accesos PCI o de E/S heredada VGA.
8. Configuración excesivamente agresiva del chipset UMC.

Varias observaciones redujeron pronto la probabilidad de las primeras hipótesis:

- el problema aparecía también con una S3 ViRGE distinta;
- los valores incorrectos seguían patrones compatibles con desincronización del DAC;
- deshabilitar cachés no resolvía el problema;
- cambiar de slot PCI, parámetros de BIOS y otras opciones comunes tampoco lo corregía;
- TURBO OFF modificaba el comportamiento (medido con herramientas de diagnóstico)

---

## 5. Primer diagnóstico del DAC VGA

Se creó una utilidad DOS de 16 bits para escribir y leer directamente el DAC mediante los puertos estándar:

```text
3C6h  PEL mask
3C7h  DAC read index
3C8h  DAC write index
3C9h  DAC data
```

Las primeras pruebas de escritura secuencial rápida mostraron grandes cantidades de errores.

El patrón era especialmente significativo: en numerosas ocasiones una entrada de paleta terminaba conteniendo valores que correspondían a la entrada esperada anterior o posterior. Es decir, aparecían secuencias del tipo:

```text
palette[n] ~= expected[n+1]
```

u otros desplazamientos similares.

Esto encajaba mucho mejor con una pérdida, repetición o mala aceptación de un ciclo de E/S que con una corrupción aleatoria de bits.

El DAC VGA mantiene internamente un estado del tipo:

```text
R -> G -> B -> incrementar índice
```

Si un acceso a `3C9h` se pierde o se duplica, el flujo completo queda desfasado y los siguientes bytes terminan interpretándose como componentes equivocados o como datos de la siguiente entrada.

Ésta fue una de las primeras pistas fuertes de que el problema estaba en la **temporización de los ciclos de E/S**.

---

## 6. Pruebas con retardos por puerto 80h

Como prueba inicial se insertaron escrituras a `OUT 80h` entre accesos al DAC.

El efecto fue muy grande:

- sin delay: decenas de miles de componentes incorrectos;
- con 200 escrituras a puerto 80h: la cantidad de errores bajó aproximadamente un **98 %**.

Esto indicaba que el problema era altamente sensible a temporización.

Sin embargo, todavía quedaban errores residuales, por lo que el delay no representaba una solución completa.

---

## 7. VGAPAL2: separar problemas de lectura y escritura

Se creó **VGAPAL2**, que permitía introducir retardos de forma independiente en:

- escritura al DAC;
- lectura del DAC.

Resultados representativos:

| Escritura | Lectura | Componentes incorrectos |
|---|---|---:|
| rápida | rápida | 24.758 |
| retardada | rápida | 20.792 |
| rápida | retardada | 4.159 |
| retardada | retardada | 327 |

Conclusiones:

- existía un problema real de **lectura**;
- existía también un problema real de **escritura**;
- los retardos reducían mucho ambos;
- la mayor parte del problema observado inicialmente durante la verificación podía amplificarse por la lectura inestable.

Otra observación importante fue que reprogramar `3C7h` antes de cada entrada de paleta impedía que un fallo puntual en lectura propagase una desincronización larga.

Esto reforzó el modelo de una máquina de estados interna del DAC perdiendo sincronización.

---

## 8. VGAPAL3: comparar estrategias de escritura

**VGAPAL3** introdujo varias estrategias:

1. `SEQ_FAST`
2. `IDX_DELAY`
3. `DATA_DELAY`
4. `RGB_DELAY`
5. `REINDEX_FAST`
6. `REINDEX_FULL_DELAY`

Se probó tanto con TURBO OFF como con TURBO ON.

### TURBO OFF

`SEQ_FAST` seguía mostrando muchos errores, pero algunas estrategias mejoraban drásticamente.

Por ejemplo:

- `SEQ_FAST`: 10.330 componentes erróneos
- `IDX_DELAY`: 32
- `REINDEX_FULL_DELAY`: 33

### TURBO ON

Los errores aumentaban de forma importante:

- `SEQ_FAST`: 11.125 errores
- `REINDEX_FAST`: 457
- `REINDEX_FULL_DELAY`: 123

La estrategia `REINDEX_FAST`, sin introducir un gran delay, reducía mucho la propagación de errores porque reestablecía constantemente el índice del DAC.

Conclusión:

> El fallo primario era probabilístico y puntual, pero el auto-incremento del DAC convertía un único error en una larga cadena de entradas desplazadas.

---

## 9. TURBO OFF como pista fundamental

Una observación práctica fue que con **TURBO OFF** los resultados obtenidos mejoraban de forma notable.

Esto podía significar dos cosas:

1. el reloj interno de la CPU bajaba;
2. la CPU seguía funcionando a una velocidad similar, pero el chipset añadía wait states o modificaba la temporización de accesos externos.

Para distinguir ambas posibilidades se creó **TURBOPROBE**.

---

## 10. TURBOPROBE: qué cambia realmente con TURBO

TURBOPROBE medía:

- operaciones aritméticas internas;
- lectura de RAM;
- escritura de RAM;
- `OUT 80h`;
- `IN 61h`;
- accesos VGA `3C8h` y `3CCh`.

### Resultados TURBO ON / TURBO OFF

Aproximadamente:

| Prueba | TURBO ON | TURBO OFF | Relación ON/OFF |
|---|---:|---:|---:|
| CPU ALU | 3,58 M/s | 3,56 M/s | 1,005x |
| RAM read | 8,39 M/s | 6,33 M/s | 1,32x |
| RAM write | 12,26 M/s | 1,26 M/s | 9,74x |
| OUT 80h | 777,8 k/s | 327,3 k/s | 2,38x |
| IN 61h | 1,325 M/s | 388,4 k/s | 3,41x |
| VGA OUT 3C8h | 776,8 k/s | 327,3 k/s | 2,37x |
| VGA IN 3CCh | 1,391 M/s | 395,7 k/s | 3,52x |

Conclusión clave:

> TURBO OFF apenas reduce el rendimiento aritmético interno de la CPU, pero ralentiza fuertemente los accesos externos.

Por tanto, el mecanismo de TURBO parecía actuar mediante **READY, wait states o temporización de bus**, no mediante una gran reducción del reloj interno del 486.

Esto explicaba también por qué los juegos se volvían lentos con TURBO OFF aunque la CPU internamente siguiera siendo rápida.

---

## 11. UMCPCI: comprobar si TURBO modifica registros PCI

Se creó **UMCPCI**, una utilidad de sólo lectura para volcar la configuración PCI del chipset UMC.

Se compararon tres estados:

```text
TURBO ON
TURBO OFF sin reiniciar
TURBO ON de nuevo
```

Los volcados fueron byte por byte idénticos salvo por la etiqueta textual usada al ejecutar la utilidad.

Esto descartó que el botón TURBO estuviera cambiando dinámicamente algún registro visible de configuración PCI.

La conclusión se desplazó todavía más hacia una modificación hardware o de temporización externa no reflejada en el espacio PCI estándar.

---

## 12. VGAPAL4: distinguir “esperar” de “cambiar el ciclo”

La siguiente pregunta fue fundamental:

> ¿TURBO OFF arregla el fallo simplemente dejando más tiempo entre dos accesos consecutivos, o modifica la forma en que se ejecuta el propio ciclo de E/S?

Para responder se creó **VGAPAL4**.

Se compararon:

- delays puramente CPU de aproximadamente 1, 2, 4 y 8 µs;
- grupos de 1, 2, 4 y 8 `OUT 80h`;
- escritura rápida sin delay.

Todos los retardos se aplicaban después de `3C8h` y de cada escritura a `3C9h`.

### Resultado con configuración original y TURBO ON

Resumen:

| Estrategia | Errores |
|---|---:|
| FAST | 8.313 |
| CPU 1 µs | 8.084 |
| CPU 2 µs | 8.102 |
| CPU 4 µs | 7.125 |
| CPU 8 µs | 6.335 |
| OUT80 x1 | 7.735 |
| OUT80 x2 | 7.498 |
| OUT80 x4 | 6.674 |
| OUT80 x8 | 5.563 |

Ni siquiera introducir aproximadamente 8,7 µs de espera CPU después de cada escritura corregía el problema.

Conclusión:

> TURBO OFF no funciona simplemente porque deje “un hueco” entre dos accesos. Debe cambiar la temporización o el handshake del propio ciclo externo.

Esto encajaba con:

- ancho del strobe;
- READY/wait states;
- aceptación del ciclo por el bridge;
- conversión CPU → PCI;
- serialización interna.

---

## 13. Primeros registros candidatos del UM8881F

A partir de documentación reconstruida del chipset se identificaron dos bits tentativamente relacionados con:

- CPU-to-PCI Post Write;
- CPU-to-PCI Burst Write.

Se creó **UMCTEST** para probarlos de forma extremadamente controlada.

Estados probados:

```text
baseline : 52=B4, 53=87
WS1      : 52=34, 53=87
NOBURST  : 52=B4, 53=07
BOTH     : 52=34, 53=07
```

### Resultado de VGAPAL4

`FAST` produjo:

| Configuración | Errores |
|---|---:|
| baseline | 8.313 |
| WS1 | 8.605 |
| NOBURST | 8.386 |
| BOTH | 8.410 |

No hubo ninguna mejora relevante.

Decisión:

> Los bits 52.7 y 53.7 no controlan el mecanismo responsable del fallo del DAC.

Se descartaron como solución.

---

## 14. Nuevo candidato: UM8886A A4[1:0]

La documentación reconstruida sugería tentativamente que `A4[1:0]` del UM8886A podía controlar una relación CPU-to-PCI del tipo:

```text
00 -> 2:1
01 -> 1:1
10 -> 3:2
11 -> reservado
```

La máquina arrancaba originalmente con:

```text
A4 = 01h
```

Se creó **UMCA4** para modificar únicamente esos dos bits de manera controlada.

La primera prueba fue:

```text
01b -> 00b
```

---

## 15. Resultado decisivo con A4=00

### TURBOPROBE

Con `A4=00`:

| Prueba | Resultado |
|---|---:|
| CPU ALU | 3,580 M/s |
| RAM read | 8,385 M/s |
| RAM write | 12,263 M/s |
| OUT 80h | 497,159 k/s |
| IN 61h | 993,282 k/s |
| VGA OUT 3C8h | 497,159 k/s |
| VGA IN 3CCh | 1,092 M/s |
| VGA 3C8 pair | 497,159 k/s |

Comparado con TURBO ON original:

- CPU: sin pérdida apreciable;
- RAM: sin pérdida apreciable;
- E/S: más lenta y conservadora;
- VGA: misma tendencia que el resto de E/S.

Esto era exactamente el tipo de cambio buscado.

### VGAPAL4

El resultado fue total:

```text
FAST      : 12/12 perfectas, 0 errores
CPU_1US   : 12/12 perfectas, 0 errores
CPU_2US   : 12/12 perfectas, 0 errores
CPU_4US   : 12/12 perfectas, 0 errores
CPU_8US   : 12/12 perfectas, 0 errores
IO80_X1   : 12/12 perfectas, 0 errores
IO80_X2   : 12/12 perfectas, 0 errores
IO80_X4   : 12/12 perfectas, 0 errores
IO80_X8   : 12/12 perfectas, 0 errores
```

La estrategia `FAST`, que antes generaba miles de errores, pasó a **cero**.

Ésta fue la prueba más fuerte de toda la investigación.

---

## 16. Validación con software real

La corrección se validó repetidamente en juegos reales.

Con `UMCA4 TEST00 APPLY` funcionaron correctamente:

- Prince of Persia
- Prince of Persia 2
- Doom
- Duke Nukem 3D
- Mortal Kombat II
- Windows 95

Al ejecutar:

```text
UMCA4 RESTORE APPLY
```

el problema reaparecía.

Se repitió varias veces el ciclo:

```text
A4=01 -> falla
A4=00 -> correcto
A4=01 -> falla
A4=00 -> correcto
```

Esto estableció una relación causal práctica muy fuerte.

---

## 17. Descubrimiento adicional: también se corrigió la red PCI

La prueba con Windows 95 reveló que el problema original no era exclusivamente gráfico.

Antes de aplicar `A4=00`:

- la RTL8029 perdía paquetes;
- la red era lenta;
- una transferencia FTP no llegaba a completarse correctamente.

Después de aplicar `A4=00`:

- la Ethernet funcionó correctamente;
- el comportamiento en Windows pasó a ser comparable al obtenido en MS-DOS con el packet driver;
- las transferencias fueron estables.

Este hallazgo cambió la interpretación final del problema:

> No se trataba de una incompatibilidad aislada con el DAC VGA, sino de una temporización PCI global demasiado agresiva que afectaba a más de un dispositivo.

La VGA era simplemente uno de los periféricos más sensibles y hacía el problema especialmente visible.

---

## 18. Prueba del estado intermedio A4=10

Se consideró la posibilidad de que `10b`, tentativamente asociado a una relación intermedia, ofreciese mejor rendimiento manteniendo la estabilidad.

Se añadió un modo experimental a UMCA4 para probar:

```text
A4[1:0] = 10b
```

Resultado:

- reaparecieron los problemas de paleta.

Decisión:

> Se descartó `10b`.

El estado `00b` quedó como única configuración probada y estable.

No se probó `11b`, al considerarse reservado o no documentado.

---

## 19. Coste de rendimiento

Se utilizó el `timedemo` de Doom para medir el impacto real de `A4=00`.

Resultados:

```text
A4=01, configuración original: 1891 realtics
A4=00, configuración estable:  2009 realtics
```

Para la misma demo:

- incremento de tiempo: aproximadamente **6,24 %**;
- rendimiento efectivo relativo: aproximadamente **94,13 %**;
- pérdida de rendimiento efectiva: aproximadamente **5,87 %**.

Se consideró una penalización razonable, dado que:

- CPU y RAM mantienen su rendimiento;
- la VGA queda completamente estable;
- la red PCI queda estable;
- TURBO OFF era muchísimo más costoso.

---

## 20. Interpretación técnica final

La explicación que mejor encaja con el conjunto de resultados es la siguiente:

1. La BIOS deja el UM8886A con `A4[1:0]=01b`.
2. Ese estado configura el camino CPU/PCI o el subsistema de E/S con una temporización demasiado agresiva para esta placa concreta.
3. Algunos ciclos PCI o de E/S heredada no se completan de forma suficientemente fiable.
4. En el DAC VGA, un único ciclo perdido o mal aceptado desincroniza la secuencia R/G/B y genera largas cadenas de colores desplazados.
5. En la RTL8029, el mismo problema se manifiesta como pérdida de paquetes o transferencias inestables.
6. TURBO OFF enmascara el problema porque introduce una temporización externa mucho más conservadora, pero con una penalización excesiva.
7. `A4=00` modifica específicamente el comportamiento de E/S/PCI sin penalizar CPU ni RAM de forma significativa.
8. Con `A4=00`, tanto el DAC VGA como la tarjeta de red funcionan correctamente.

No se ha medido directamente el reloj PCI, por lo que no puede afirmarse de forma absoluta que `A4=00` signifique exactamente “PCI a 16,7 MHz” o que `A4=01` sea exactamente “33 MHz”. La interpretación `2:1 / 1:1 / 3:2` procede de documentación reconstruida y debe seguir considerándose tentativa.

Lo que sí está demostrado empíricamente es el efecto funcional del registro.

---

## 21. Solución definitiva: UMCFIX

Una vez identificada la configuración estable se creó **UMCFIX**, separando claramente:

- **UMCA4**: herramienta de laboratorio y diagnóstico;
- **UMCFIX**: solución mínima para uso diario.

UMCFIX está pensada para ejecutarse desde `AUTOEXEC.BAT`.

Su comportamiento es deliberadamente conservador:

1. Detecta el chipset esperado.
2. Comprueba revisiones y valores conocidos.
3. Lee `UM8886A:A4h`.
4. Si ya está en `00h`, no hace nada.
5. Si está en `01h`, modifica únicamente `A4[1:0]` para llevarlo a `00b`.
6. Verifica la escritura.
7. Si encuentra un estado inesperado, no escribe.

Uso recomendado:

```bat
C:\UTIL\UMCFIX.EXE /Q
```

También dispone de modo de comprobación:

```bat
UMCFIX /CHECK
```

La idea es que el sistema arranque con su BIOS original y que UMCFIX aplique inmediatamente la temporización estable antes de iniciar juegos, drivers, red o Windows 95.

---

## 22. Decisiones finales

Se tomaron las siguientes decisiones:

- Mantener `A4[1:0]=00b` como solución estable.
- Descartar `A4[1:0]=10b` por reproducir la corrupción de paleta.
- No probar `11b` por ser desconocido o reservado.
- No usar los bits 52.7 y 53.7 del UM8881F como solución.
- No utilizar TURBO OFF como workaround permanente por su coste de rendimiento.
- Mantener UMCA4 como herramienta de diagnóstico.
- Utilizar UMCFIX como solución permanente desde `AUTOEXEC.BAT`.
- No modificar todavía la BIOS de la placa, ya que la solución software es sencilla, reversible y suficientemente robusta.

---

## 23. Conclusión

La investigación comenzó como un problema de paleta VGA y terminó identificando un defecto de temporización mucho más general en el subsistema PCI de la PCChips M915i.

El punto de inflexión fue comprobar que TURBO OFF afectaba principalmente a los accesos externos y no al rendimiento interno de la CPU. A partir de ahí, las pruebas fueron acotando el problema hasta demostrar que los delays software no bastaban: era necesario modificar la propia temporización del bus.

El cambio controlado de `UM8886A:A4[1:0]` de `01b` a `00b` produjo una corrección completa y reproducible:

- cero errores en las pruebas de DAC;
- paletas correctas en todos los juegos probados;
- Windows 95 estable;
- RTL8029 estable;
- CPU y RAM a velocidad normal;
- sólo una penalización moderada de rendimiento real.

Por tanto, la solución final adoptada es:

```text
UM8886A A4[1:0] = 00b
```

aplicada automáticamente mediante **UMCFIX** durante el arranque de DOS.

La evidencia disponible permite considerar esta configuración como la solución práctica al defecto crítico de temporización PCI de esta máquina concreta.
