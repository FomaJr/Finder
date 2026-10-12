# Finder

GUI-утилита для поиска слова, фразы или маски в файлах папки.

![Platform](https://img.shields.io/badge/platform-Windows%20x64-blue)
![C++](https://img.shields.io/badge/C%2B%2B-17-orange)
![License](https://img.shields.io/badge/license-MIT-green)

## Возможности

- Рекурсивный поиск по содержимому файлов в папке
- Маски (`*`, `?`) и обычные подстроки
- Игнор регистра
- Прогресс-бар и статус «N из M файлов»
- Асинхронный поиск — окно не «зависает» во время работы
- Результаты: двойной клик — открыть файл в подходящем редакторе
- Правый клик по результату: копировать или открыть файл
- Кнопка «Сохранить в .txt» — выгрузить результаты
- Сохранение настроек между запусками (`settings.ini` рядом с `.exe`)
- Корректное закрытие во время поиска

## Какие файлы ищет

- **Текстовые и код**: `.txt .log .md .ini .cfg .conf .json .xml .yml .yaml .csv .tsv .tex .rtf`
- **Код**: `.cpp .c .h .hpp .cc .cxx .cs .java .py .js .ts .jsx .tsx .php .rb .go .rs .lua .pl .sh .bat .cmd .ps1 .html .htm .css .scss .less .sql .vue .kt .swift .m .mm .asm .s .gradle .properties`
- **Офисные (ZIP-формат)**: `.docx .docm .xlsx .xlsm .pptx .pptm .odt .ods .odp`
- **Офисные (старый формат)**: `.doc .xls .ppt`
- **PDF**: `.pdf` — через утилиту [Poppler](https://poppler.freedesktop.org/) `pdftotext.exe`

Остальные файлы (изображения, видео, архивы, `.exe`) пропускаются.

## Скачать

Готовый архив — на странице [**Releases**](../../releases/latest).

Рекомендуется скачивать **`Finder-portable.zip`** — внутри лежит `Finder.exe`, `pdftotext.exe` и все нужные библиотеки. Распакуйте архив в любую папку — и запускайте.

Если PDF-поиск не нужен, можно скачать только `Finder.exe` — он самодостаточен, но PDF искать не будет.

Прямая ссылка на последний portable-архив:

https://github.com/FomaJr/Finder/releases/latest/download/Finder-portable.zip


## Использование

1. Запустите `Finder.exe`.
2. Укажите папку для поиска (кнопка **Обзор…**).
3. Введите слово, фразу или маску.
4. При необходимости включите **Маска** (`*`, `?`) и **Без учёта регистра**.
5. Нажмите **Найти**.
6. Двойной клик по строке результата — открыть файл.

## Системные требования

- **Windows 10 / 11 x64** — работает из коробки
- **Windows 7 / 8.1 x64** — нужно установить [Visual C++ Redistributable 2015–2022 (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe)
- **32-битная Windows, Windows XP** — не поддерживается

## Сборка из исходников

Требуется [MSYS2](https://www.msys2.org/) UCRT64 с пакетами `mingw-w64-ucrt-x86_64-gcc` и `mingw-w64-ucrt-x86_64-zlib`.

```bash
g++ -std=c++17 -O2 -static -s -mwindows -municode Finder.cpp -o Finder.exe -lcomctl32 -lole32 -lshell32 -lz