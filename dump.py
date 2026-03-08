import os
import json
import subprocess

BLACKLIST_FILES = [
    "gradlew",
    "gradlew.bat",
    "LICENSE",
    "icon.png",
    "icon_256x.png",
]

BLACKLIST_FOLDERS = [
    ".git",
    ".vscode",
    ".gradle",
    ".idea",
    "dev",
    "dist",
    "test",
    "build",
    "gradle",
]


def requestDir(base_dir):
    dir_input = input(
        "Enter the directory you want to code-dump (e.g. './'):\n> "
    )
    return os.path.abspath(os.path.join(base_dir, dir_input))


def getTree(directory):
    try:
        tree_output = subprocess.check_output(["tree", directory], text=True)
        return tree_output
    except FileNotFoundError:
        return ("Error: 'tree' command not found. Please ensure it is installed "
                "on your system.")
    except subprocess.CalledProcessError as e:
        return f"Error generating directory tree: {e}"


def getCode(directory):
    json_data = {}
    md_content = ""

    tree_output = getTree(directory)
    md_content += tree_output + "\n"

    for root, _, files in os.walk(directory):
        rel_root = os.path.relpath(root, directory)
        path_components = rel_root.split(os.sep)

        is_blacklisted_folder = False
        curr_path = ""
        for component in path_components:
            curr_path = os.path.join(curr_path, component) if curr_path else component
            if curr_path in BLACKLIST_FOLDERS and curr_path != ".":
                is_blacklisted_folder = True
                break
        if rel_root in BLACKLIST_FOLDERS and rel_root != ".":
            is_blacklisted_folder = True
        if is_blacklisted_folder:
            continue

        curr_json = json_data
        if rel_root != ".":
            for component in rel_root.split(os.sep):
                if component not in curr_json:
                    curr_json[component] = {}
                curr_json = curr_json[component]

        for file in files:
            file_path = os.path.join(root, file)
            file_rel_path = os.path.relpath(file_path, directory)

            if file in BLACKLIST_FILES or file_rel_path in BLACKLIST_FILES:
                continue

            _, ext = os.path.splitext(file)
            file_type = ext[1:] if ext else "unknown"

            try:
                with open(file_path, "r", encoding="utf-8") as f:
                    code_content = f.read()
            except Exception as e:
                code_content = f"Error reading file: {e}"

            curr_json[file] = {"type": file_type, "code": code_content}
            md_content += f"\n### ./{file_rel_path}:\n"
            md_content += f"```{file_type}\n{code_content}\n```\n"

    return md_content, json_data


if __name__ == "__main__":
    script_dir = os.path.dirname(os.path.abspath(__file__))

    output_json_path = os.path.join(script_dir, "project.json")
    output_md_path = os.path.join(script_dir, "project.md")

    base_dir = os.path.abspath(os.path.join(script_dir, ".."))

    while True:
        target_dir = requestDir(base_dir)
        if not os.path.exists(target_dir):
            print("Please enter a valid directory!")
        else:
            break

    print(f"Dumping all code from {target_dir} into\n  {output_json_path}\nand\n  "
          f"{output_md_path}")

    mdDump, jsonDump = getCode(target_dir)

    try:
        with open(output_json_path, "w", encoding="utf-8") as f_json:
            json.dump(jsonDump, f_json, indent=4)
    except Exception as e:
        print(f"Error writing to project.json: {e}")

    try:
        with open(output_md_path, "w", encoding="utf-8") as f_md:
            f_md.write(mdDump)
    except Exception as e:
        print(f"Error writing to project.md: {e}")

    print(f"Successfully dumped code from {target_dir}")
