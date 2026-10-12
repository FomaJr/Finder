# Finder

Простая утилита для Windows: ищет слово, фразу или маску во всех файлах выбранной папки.

## Что ищет

Обычные текстовые файлы и код: .txt, .log, .md, .cpp, .py, .html, .json, .csv и другие. Документы Word: .docx, .doc, .rtf, .odt. Таблицы: .xlsx, .xls, .ods. Презентации: .pptx, .ppt, .odp. PDF — если рядом с программой лежит pdftotext.exe. Картинки, видео, архивы и .exe пропускаются.

## Скачать

Из раздела Releases. Основной вариант — Finder-portable.zip, там уже лежит всё нужное: сама программа, утилита для PDF и библиотеки к ней. Распакуйте архив и запустите Finder.exe. Если PDF искать не нужно — можно взять только Finder.exe.

## Как пользоваться

Запустить Finder.exe. Выбрать папку или сразу файл. Ввести, что искать. Нажать «Найти». Двойной клик по результату открывает файл: текстовые — в Блокноте, документы — в Word, PDF — в Adobe Reader или чём у вас ассоциировано.

## Требования

Windows 10 или 11, 64 бита. На Windows 7 и 8.1 тоже пойдёт, но может потребоваться VC++ Redistributable. 32-битная Windows не поддерживается.

## Сборка

Нужны MSYS2 UCRT64, компилятор g++ и zlib:

pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-zlib

Команда сборки:

g++ -std=c++17 -O2 -static -s -mwindows -municode Finder.cpp -o Finder.exe -lcomctl32 -lole32 -lshell32 -lz

Для работы PDF-поиска рядом с Finder.exe должен лежать pdftotext.exe из poppler-windows — вместе со всеми DLL.

## Лицензия

MIT.