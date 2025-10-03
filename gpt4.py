import os
import re
import sys

# Этап 1. Обход каталогов (os.walk)

#     Правило D-1 — EXCLUDE_DIRS
#     Если относительный путь текущего каталога (rel_root) есть в EXCLUDE_DIRS,

#         исключаем всю папку:
#         • его файлы не анализируем;
#         • dirs[:] = [] — в подкаталоги не заходим.
#         Иначе → следующий шаг.

#     Правило D-2 — фильтрация подпапок
#     Из списка dirs убираем те имена, которые в сочетании rel_root/dir попадают в EXCLUDE_DIRS.
#     Удалённые каталоги будут пропущены целиком; по оставшимся os.walk продолжит обход.

#     Результат: к файловому фильтру попадут только файлы из разрешённых каталогов.

# Этап 2. Фильтр файлов (should_include)

# Проверки идут сверху-вниз; как только условие выполнено — решение окончательно.

#     F-1 — EXCLUDE_FILENAMES (точное имя)
#     Совпало → файл исключён, к следующим правилам не переходим.
#     Иначе → F-2.

#     F-2 — EXCLUDE_EXTENSIONS (расширение)
#     Совпало → исключаем.
#     Иначе → F-3.

#     F-3 — EXCLUDE_REGEXES (regex по полному имени)
#     Любой паттерн совпал → исключаем.
#     Иначе → F-4.

#     F-4 — INCLUDE_FILENAMES (точное имя)
#     Совпало → файл включён.
#     Иначе → F-5.

#     F-5 — INCLUDE_EXTENSIONS (расширение)
#     Совпало → включаем.
#     Иначе → F-6.

#     F-6 — INCLUDE_REGEXES (regex по полному имени)
#     Совпало → включаем.
#     Иначе → F-7.

#     F-7 — INCLUDE_SHEBANG
#     Если расширения нет и первая строка начинается с #! → включаем.
#     Иначе → F-8.

#     F-8 — правило по умолчанию
#     Ни одно условие не сработало → файл исключён.



    # Каталоги-исключения отсекаются раньше всего — в них даже не смотрим файлы.

    # Внутри разрешённых каталогов любой файл сначала проходит «чёрные списки» (F-1 → F-3).

    # Если ни один «чёрный» триггер не сработал, вступают «белые списки» (F-4 → F-6).

    # Файлы без расширения получают последний шанс через шебанг (F-7).

    # Отсутствие срабатываний означает исключение (F-8).
    # Это можно изменить, если вернуть True вместо False в самом конце функции should_include


# ───--- НАСТРОЙКИ ---───
INCLUDE_EXTENSIONS   = {'.c', '.h'}            # включать файлы с этими расширениями
EXCLUDE_EXTENSIONS   = {'.o', '.md', '.data', 'js', 'wasm', 'txt'}   # исключать файлы с этими расширениями

# точные имена файлов для включения
INCLUDE_FILENAMES    = {'Makefile'}
EXCLUDE_FILENAMES    = {'output.md', 'README.org', '.emscripten'}              # точные имена файлов для исключения

INCLUDE_REGEXES = [                       # regex-паттерны для включения (имя и расширение)
    re.compile(r'^\d+$'),                 #   — только цифры
]
EXCLUDE_REGEXES = [                       # regex-паттерны для исключения
    # re.compile(r'^tmp_'),               # пример: исключить всё, что начинается с tmp_
    re.compile(r'^gpt\d+.py$'),
]

INCLUDE_SHEBANG = True                    # брать скрипты без расширения с #! в первой строке

EXCLUDE_DIRS = {                          # пути каталогов, которые пропускаем целиком
    '.emscripten_cache',
    '.git',
    'web',
    'src/core',
    'src/gfx',
    'src/apps',
    # 'src/include',

}

# Файлы, которые включаем всегда, даже если их каталоги в EXCLUDE_DIRS
ALWAYS_INCLUDE_FILES = {
    'src/apps/console_processor.c',
    'src/apps/console_processor_ext.c',
    'src/apps/console_sink.c',
    # 'src/apps/console_store.c',
    # 'src/apps/win_console.c',
}

# ───────────────────────


def should_include(fname: str, dpath: str) -> bool:
    """
    Определяет, попадёт ли файл в итоговый markdown.
    Приоритет: точные исключения → расширения-исключения → regex-исключения →
               точные включения → расширения-включения → regex-включения → shebang.
    """
    name, ext = os.path.splitext(fname)

    # 1) Полное имя в списке исключений
    if fname in EXCLUDE_FILENAMES:
        return False

    # 2) Расширение в списке исключений
    if ext in EXCLUDE_EXTENSIONS:
        return False

    # 3) Regex-исключения по имени c расширением
    if any(rx.fullmatch(fname) for rx in EXCLUDE_REGEXES):
        return False

    # 4) Полное имя в списке включений
    if fname in INCLUDE_FILENAMES:
        return True

    # 5) Расширение в списке включений
    if ext and ext in INCLUDE_EXTENSIONS:
        return True

    # 6) Regex-включения
    if any(rx.fullmatch(fname) for rx in INCLUDE_REGEXES):
        return True

    # 7) Скрипты без расширения, начинающиеся с shebang
    if INCLUDE_SHEBANG and ext == '':
        try:
            with open(os.path.join(dpath, fname), 'r', encoding='utf-8', errors='ignore') as f:
                return f.readline(256).startswith('#!')
        except (OSError, IOError):
            return False

    return False


def concatenate_files(output_file: str = 'output.md') -> None:
    root_start = os.path.abspath('.')      # нужна для корректного относительного пути
    included_abs = set()                   # чтобы не задублировать, если файл уже попал из разрешённых директорий

    with open(output_file, 'w', encoding='utf-8') as out:
        for root, dirs, files in os.walk('.'):
            # --- Фильтрация подкаталогов -------------------------------
            rel_root = os.path.relpath(root, '.')  # '.' для самой корневой итерации
            # Если сам текущий каталог исключён, не обрабатываем файлы и не углубляемся
            if rel_root in EXCLUDE_DIRS:
                dirs[:] = []               # остановить обход глубже
                continue

            # Удаляем из dirs те подпапки, что числятся в EXCLUDE_DIRS
            dirs[:] = [
                d for d in dirs
                if os.path.normpath(os.path.join(rel_root, d)) not in EXCLUDE_DIRS
            ]
            # -----------------------------------------------------------

            for file in files:
                if should_include(file, root):
                    path = os.path.join(root, file)

                    size = os.path.getsize(path)
                    rel  = os.path.relpath(path, root_start)
                    print(f"{size:>10} {rel}", file=sys.stderr)

                    with open(path, 'r', encoding='utf-8', errors='ignore') as inp:
                        out.write('```\n')
                        out.write(f'// {os.path.relpath(path, root_start)}\n')
                        out.write(inp.read())
                        out.write('\n```\n\n')

        # ── ДОКЛЕЙКА: добавляем явные файлы из исключённых каталогов ──
        for rel_path in ALWAYS_INCLUDE_FILES:
            abs_path = os.path.abspath(rel_path)
            if abs_path in included_abs:
                continue  # уже добавлен обычным путём

            try:
                size = os.path.getsize(abs_path)
            except OSError:
                # файл не найден — пропускаем (можно залогировать)
                continue

            rel = os.path.relpath(abs_path, root_start)
            print(f"{size:>10} {rel}", file=sys.stderr)

            with open(abs_path, 'r', encoding='utf-8', errors='ignore') as inp:
                out.write('```\n')
                out.write(f'// {rel}\n')
                out.write(inp.read())
                out.write('\n```\n\n')

if __name__ == '__main__':
    concatenate_files()
