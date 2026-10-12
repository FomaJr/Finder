# Finder

GUI-утилита для поиска слова, фразы или маски в файлах папки.

![Platform](https://img.shields.io/badge/platform-Windows%20x64-blue)
![C++](https://img.shields.io/badge/C%2B%2B-17-orange)
![License](https://img.shields.io/badge/license-MIT-green)

## Возможности

- Рекурсивный поиск по содержимому файлов
- Маски (`*`, `?`) и обычные подстроки
- Игнор регистра
- Фильтр по расширениям (например `.cpp,.h`)
- Асинхронный поиск — окно не «зависает»

## Скачать

Готовый `Finder.exe` — на странице [**Releases**](../../releases/latest).

Прямая ссылка на последнюю версию:

https://github.com/FomaJr/Finder/releases/latest/download/Finder.exe


## Использование

1. Запустите `Finder.exe`
2. Укажите папку (или файл) для поиска
3. Введите слово, фразу или маску
4. При необходимости включите «Маска» / «Без учёта регистра» и задайте расширения
5. Нажмите **Найти**

## Системные требования

- **Windows 10 / 11 x64** — работает «из коробки»
- **Windows 7 / 8.1 x64** — может потребоваться [VC++ Redistributable 2015–2022](https://aka.ms/vs/17/release/vc_redist.x64.exe)
- На 32-битной Windows и Windows XP не запустится

## Сборка из исходников

Требуется [MSYS2](https://www.msys2.org/) с пакетом `mingw-w64-ucrt-x86_64-gcc`.

```bash
g++ -std=c++17 -O2 -static -s -mwindows -municode Finder.cpp -o Finder.exe -lcomctl32 -lole32 -lshell32