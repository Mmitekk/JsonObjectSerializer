# JsonObjectSerializer

A **Unreal Engine 5.8** plugin providing Blueprint nodes for deep serialization of `UObject` to JSON string and back. Uses **C++ Reflection** to iterate all `UPROPERTY` fields, including recursive handling of nested `UObject*` and `TArray`.

---

## Blueprint Nodes

### Make Json From Object

> Serializes a UObject to a JSON string using C++ Reflection. Recursively handles nested UObject* and TArray. Adds a __ObjectClassPath field to JSON for class reconstruction during deserialization.

**Pins:**
- **Input:** `Target` (UObject*) — object to serialize
- **Output:** `JsonString` (FString) — result as JSON, `Success` (bool) — operation result

### Spawn Object From Json

> Deserializes a JSON string into a new UObject created via NewObject. Class is determined by the __ObjectClassPath field. Recursively handles nested UObject* and TArray. Outer is used as the owner for the object hierarchy.

**Pins:**
- **Input:** `Outer` (UObject*) — owner for object hierarchy, `InJsonString` (FString) — JSON string to deserialize
- **Output:** `SpawnedObject` (UObject*) — created object, `Success` (bool) — operation result

---

## Important: First-Time Compilation

This is a **C++ plugin**. Before you can use it in a Blueprint-only project, you **must compile it once** in a C++ project. Here is the step-by-step process:

### Step 1. Create a temporary C++ project

1. Open **Unreal Engine 5.8** → click **New Project**
2. Select the **Games** category → choose **Blank**
3. Under project settings, select **C++** (NOT Blueprint)
4. Name the project (e.g., `TempBuildProject`) and click **Create**

> **Why?** Blueprint-only projects do not have Visual Studio integration or Source folders, so the C++ plugin cannot be compiled in them. You need a C++ project to build the plugin DLL.

### Step 2. Add the plugin to the project

1. Close the Unreal Editor
2. Navigate to the project folder on disk
3. Create a `Plugins` folder inside the project (if it doesn't exist)
4. Clone this repository into the `Plugins` folder:
   ```
   cd TempBuildProject/Plugins
   git clone https://github.com/Mmitekk/JsonObjectSerializer.git
   ```

### Step 3. Generate project files

1. Right-click the `.uproject` file → **Generate Visual Studio project files**

### Step 4. Compile the plugin

1. Double-click the `.sln` file to open **Visual Studio**
2. Set the solution configuration to **Development Editor** (top toolbar)
3. In Solution Explorer, right-click the project → **Build**
4. Wait for the build to complete — you should see `Build: 1 succeeded, 0 failed`

> **If the build fails**, make sure:
> - You are using UE 5.8
> - Visual Studio has the "Game development with C++" workload installed
> - Delete the `Intermediate` folder in the project root and retry

### Step 5. Verify the plugin works

1. Open the project in **Unreal Editor**
2. Go to **Edit → Plugins** → find **Json Object Serializer** — it should be enabled
3. Open any Blueprint → right-click → search for **Make Json From Object** and **Spawn Object From Json**
4. If both nodes appear — the plugin is working

### Step 6. Copy the compiled plugin to your Blueprint project

Now that the plugin is compiled, you can copy it to any project (including Blueprint-only ones):

1. Go to the compiled project folder:
   ```
   TempBuildProject/Plugins/JsonObjectSerializer/
   ```
2. Copy the **entire** `JsonObjectSerializer` folder to your target project's `Plugins` folder:
   ```
   YourBlueprintProject/Plugins/JsonObjectSerializer/
   ```
3. Open your Blueprint project in Unreal Editor
4. Go to **Edit → Plugins** → enable **Json Object Serializer**
5. The nodes **Make Json From Object** and **Spawn Object From Json** are now available

> **Important:** The compiled `Binaries` and `Intermediate` folders inside the plugin contain the DLL. If you copy only the `Source` folder without `Binaries`, the plugin will not work in a Blueprint-only project.

---

## How It Works

The plugin solves the problem of saving nested `UObject` data (e.g., stat objects inside a `UActorComponent`) that are destroyed when the game ends and are not covered by the standard `SaveGame` system.

### Serialization — `Make Json From Object`

1. Iterates all `UPROPERTY` fields of the object (except those marked `Transient`).
2. Determines the type of each field via C++ Reflection (`CastField<FBoolProperty>`, `CastField<FIntProperty>`, etc.).
3. Writes the variable name and its value to the JSON object.
4. When a `UObject*` is found — **recursively** serializes it as a nested JSON object (with its own `__ObjectClassPath`).
5. When a `TArray` is found — serializes it as a JSON array, recursively processing each element.
6. Adds a hidden field `__ObjectClassPath` to the root of the JSON object with the value `GetClass()->GetPathName()` — the class path for subsequent reconstruction.
7. Protection against infinite recursion: `TSet<const UObject*>` for cycle detection and a depth limit of 32 levels.

### Deserialization — `Spawn Object From Json`

1. Parses the JSON string via `TJsonReader` / `FJsonSerializer`.
2. Iterates `FJsonObject::Values` to extract `__ObjectClassPath`, loads the class via `LoadClass<UObject>`.
3. Creates an instance via `NewObject<UObject>(Outer, LoadedClass)`.
4. Builds a property map (`TMap<FName, FProperty*>`) and iterates JSON values, matching keys to properties by name.
5. When a `UObject*` property with a nested JSON object is found — **recursively** creates the nested object, passing the current object as `Outer`.
6. When a `TArray` is found — clears the array and fills it with elements from the JSON array via `FScriptArrayHelper`.

---

## Usage Example

### Save Workflow

```
┌──────────────────┐      ┌──────────────────────┐      ┌────────────────────┐
│  MyStatsObject   │ ───> │  Make Json From Object│ ───> │  JsonString        │
│  (UCharacterStats)│      │                      │      │  (FString)         │
└──────────────────┘      └──────────────────────┘      └────────┬───────────┘
                                                                  │
                                                                  v
                                                        ┌────────────────────┐
                                                        │  SaveGame Array    │
                                                        │  (Array of Strings)│
                                                        └────────────────────┘
```

1. Call **Make Json From Object**, passing your `UObject` (e.g., a stats object).
2. Get `JsonString` as output.
3. Save the string to `SaveGame` (as an element of an `FString` array or a single string).
4. Write `SaveGame` to a slot via **Save Game to Slot**.

### Load Workflow

```
┌────────────────┐      ┌──────────────────────┐      ┌──────────────────┐
│  SaveGame Slot │ ───> │  JsonString          │ ───> │ Spawn Object     │
│                │      │  (FString)           │      │  From Json       │
└────────────────┘      └──────────────────────┘      │  Outer:          │
                                                       │  MyComponent     │
                                                       └───────┬──────────┘
                                                               │
                                                               v
                                                       ┌──────────────────┐
                                                       │  SpawnedObject   │
                                                       │  (UCharacterStats)│
                                                       └──────────────────┘
```

1. Load `SaveGame` from a slot via **Load Game from Slot**.
2. Extract the JSON string from `SaveGame`.
3. Call **Spawn Object From Json**, passing `Outer` (e.g., your `UActorComponent`) and `JsonString`.
4. Get the restored `UObject*` with all nested objects properly bound by the `Outer` hierarchy.

### Nested Objects Example

Create three Blueprint classes (parent: Object):

- **BP_StatEntry** with variables: `StatName` (String), `StatValue` (Float)
- **BP_CharacterStats** with variables: `Level` (Integer), `Health` (Float), `Entries` (Array of BP_StatEntry*)
- **BP_StatsComponent** (ActorComponent) with variable: `Stats` (BP_CharacterStats*)

**Serialization** of `Stats` via *Make Json From Object* produces JSON:

```json
{
  "__ObjectClassPath": "/Script/MyGame.BP_CharacterStats_C",
  "Level": 5,
  "Health": 100.0,
  "Entries": [
    {
      "__ObjectClassPath": "/Script/MyGame.BP_StatEntry_C",
      "StatName": "Strength",
      "StatValue": 15.0
    },
    {
      "__ObjectClassPath": "/Script/MyGame.BP_StatEntry_C",
      "StatName": "Agility",
      "StatValue": 12.0
    }
  ]
}
```

**Deserialization** via *Spawn Object From Json* with `Outer = BP_StatsComponent` creates:

- `BP_CharacterStats` (Outer = `BP_StatsComponent`)
- `BP_StatEntry` "Strength" (Outer = `BP_CharacterStats`)
- `BP_StatEntry` "Agility" (Outer = `BP_CharacterStats`)

---

## Supported Data Types

| C++ Type         | UE Property Type     | JSON Representation |
|------------------|----------------------|---------------------|
| `bool`           | `FBoolProperty`      | Boolean             |
| `int32`          | `FIntProperty`       | Number              |
| `int64`          | `FInt64Property`     | Number              |
| `uint8`          | `FByteProperty`      | Number              |
| `uint16`         | `FNumericProperty`   | Number              |
| `uint32`         | `FNumericProperty`   | Number              |
| `uint64`         | `FNumericProperty`   | Number*             |
| `float`          | `FFloatProperty`     | Number              |
| `double`         | `FDoubleProperty`    | Number              |
| `FString`        | `FStrProperty`       | String              |
| `FName`          | `FNameProperty`      | String              |
| `FText`          | `FTextProperty`      | String**            |
| `enum` (uint8)   | `FByteProperty`      | Number              |
| `enum` (int+)    | `FEnumProperty`      | Number              |
| `UObject*`       | `FObjectProperty`    | Object (recursive)  |
| `TArray<T>`      | `FArrayProperty`     | Array (recursive)   |

> \* `uint16`, `uint32`, `uint64` are handled via `FNumericProperty` (since `FUInt16Property`, `FUInt32Property`, `FUInt64Property` were removed in UE 5.4+). Values are serialized as signed integers via `GetSignedIntPropertyValue()`, which is correct for values up to 2^53 in JSON. For `uint64` values above 2^53, precision loss is possible.
>
> \*\* `FText` is serialized via `ToString()` and restored via `FText::FromString()`. The localization key and namespace are lost.

---

## LIMITATIONS: Unsupported Data Types

The following data types **CANNOT** be properly serialized and deserialized by this plugin. When encountered, the property is skipped.

### Actor Pointers and External Components

**Why:** `AActor` is created via `AActor::SpawnActor()`, not `NewObject()`. The plugin uses `NewObject` to create objects, which is impossible for Actors. The plugin also explicitly rejects `AActor`-derived classes during deserialization. An external `UActorComponent` belonging to another Actor is also unavailable after restart: its lifecycle is tied to the Owner-Actor, which may not yet exist.

**Solution:** Use identifiers (ID, Tag, GUID) to reference Actors and Components. Save IDs as `FString` or `int32`, and on load, find the object by ID via `UGameplayStatics::GetAllActorsWithTag`, `FindActorByName`, or a custom registry system.

### Assets (UTexture2D*, UMaterialInterface*, USkeletalMesh*, USoundWave*, etc.)

**Why:** Assets are heavy resources loaded from disk packages (`.uasset`). Their contents (texture pixels, mesh vertices, audio data) cannot and should not be packed into JSON: a single 1024x1024 texture is megabytes of data. Additionally, assets are managed by `FStreamableManager` and have a special lifecycle incompatible with simple `NewObject`.

**Solution:** Use `TSoftObjectPtr<T>` or `FString` with the asset path (e.g., `"/Game/Textures/T_PlayerSkin"`). On load, use `TSoftObjectPtr::LoadSynchronous()` or async loading via `FStreamableManager`. The plugin does not serialize `TSoftObjectPtr` automatically, but you can add an `FString` field with the path and convert it in your code.

### DataAssets (UDataAsset*)

**Why:** `UDataAsset` is static editor content created during development. Its data is immutable at runtime (with rare exceptions). Serializing the entire DataAsset contents into JSON duplicates data already on disk in the `.uasset` file. On load, you would need to recreate the DataAsset via `NewObject`, but UE expects DataAssets to be loaded from packages, not created dynamically.

**Solution:** Save only the reference (path) to the DataAsset as `FSoftObjectPtr` or `FString`. On load, use `LoadObject<UDataAsset>()` or `TSoftObjectPtr` to get the existing instance.

### TMap and TSet

**Why:** Unreal Engine Reflection does not provide a standardized API for iterating and modifying `TMap` and `TSet` like it does for `TArray` (which has `FScriptArrayHelper`). While `FMapProperty` and `FSetProperty` exist in Reflection, they lack public helpers that would allow safely iterating and adding elements without knowledge of the internal hash table structure.

**Solution:**
- **For TMap:** Use an array of `UObject*`, where each `UObject` contains a key-value pair (e.g., `BP_MapEntry` with `Key` and `Value` variables). Alternatively — two parallel arrays: `TArray<FString> Keys` + `TArray<float> Values`.
- **For TSet:** Use `TArray<T>` — uniqueness can be ensured logically.

### Delegates

**Why:** Delegates are bound to specific functions and objects in memory (member function pointers, `UObject*` self). After a game restart, object addresses and functions may change. Serializing a delegate is an attempt to save an in-memory function pointer, which is meaningless after the process restarts.

**Solution:** Do not save delegates. Re-create bindings programmatically on load (in `BeginPlay`, `OnConstruct`, or after deserialization).

### Structs (FStruct / UStruct)

**Why:** The plugin is specifically designed for `UObject` — objects with their own lifecycle, `Outer` hierarchy, and the ability to be created via `NewObject`. `FStruct` (UStruct-structs) are value types embedded directly in the owner's memory. For struct serialization, UE5 already has a standard tool — `FJsonObjectConverter`, which correctly handles all struct field types. Adding `FStruct` support to this plugin would duplicate logic and complicate the code without adding new functionality.

**Solution:** Use `FJsonObjectConverter` for struct serialization, or wrap the struct in a `UObject` wrapper (add `UPROPERTY() UMyWrapper*` instead of `UPROPERTY() FMyStruct`).

---

## UE 5.8 Compatibility

The plugin is adapted for **Unreal Engine 5.8** API changes:

- **Removed property types:** `FUInt16Property`, `FUInt32Property`, `FUInt64Property` were removed in UE 5.4+. Unsigned integers are handled via `FNumericProperty::IsInteger()` + `GetSignedIntPropertyValue()` / `SetIntPropertyValue()`.
- **Removed methods:** `FNumericProperty::IsUnsignedInt()` and `FNumericProperty::SetUnsignedIntPropertyValue()` were removed. `SetIntPropertyValue()` is used instead for all integer types (signed and unsigned — bit representation matches).
- **Changed FJsonObject API:** `GetField()`, `HasField()`, `TryGetStringField()` changed signatures (transition to `FStringView` and `UE::FSharedString` as `Values` keys). Deserialization iterates `FJsonObject::Values` directly and builds a `TMap<FName, FProperty*>` property map for matching, completely bypassing compatibility issues.
- **Removed header:** `UObject/NumericProperty.h` no longer exists — `FNumericProperty` is defined in `UObject/UnrealType.h`.

---

## Plugin File Structure

```
JsonObjectSerializer/
├── JsonObjectSerializer.uplugin
├── README.md
└── Source/
    └── JsonObjectSerializerPlugin/
        ├── JsonObjectSerializerPlugin.Build.cs
        ├── Public/
        │   ├── JsonObjectSerializerPlugin.h
        │   └── JsonObjectSerializerBPLibrary.h
        └── Private/
            ├── JsonObjectSerializerPlugin.cpp
            └── JsonObjectSerializerBPLibrary.cpp
```

> **Note:** The plugin module is named `JsonObjectSerializerPlugin` (not `JsonObjectSerializer`) to avoid name collision with the project's Build.cs class.

---

## Technical Details

| Parameter                      | Value                                       |
|-------------------------------|----------------------------------------------|
| Target UE Version             | 5.8                                          |
| Module Type                   | Runtime                                      |
| Dependencies                  | `Json`, `JsonUtilities`, `CoreUObject`       |
| Max Recursion Depth           | 32                                           |
| Cycle Detection               | `TSet<const UObject*>` during serialization  |
| JSON Format                   | Compact (no whitespace, minimal size)        |
| Object Creation               | `NewObject<UObject>(Outer, LoadedClass)`     |
| Class Loading                 | `LoadClass<UObject>(nullptr, ClassPath)`     |
| JSON Field Access             | Iteration of `FJsonObject::Values` (UE 5.8)  |

---
---

# JsonObjectSerializer (Русская версия)

Плагин для **Unreal Engine 5.8**, предоставляющий Blueprint-ноды для глубокой сериализации `UObject` в JSON-строку и десериализации обратно. Использует **C++ Reflection** для перебора всех `UPROPERTY`-полей, включая рекурсивную обработку вложенных `UObject*` и `TArray`.

---

## Blueprint-ноды

### Make Json From Object

> Сериализует UObject в JSON-строку через C++ Reflection. Рекурсивно обрабатывает вложенные UObject* и TArray. В JSON добавляется поле __ObjectClassPath для восстановления класса при десериализации.

**Пины:**
- **Вход:** `Target` (UObject*) — объект для сериализации
- **Выход:** `JsonString` (FString) — результат в формате JSON, `Success` (bool) — успешность операции

### Spawn Object From Json

> Десериализует JSON-строку в новый UObject, созданный через NewObject. Класс определяется по полю __ObjectClassPath. Рекурсивно обрабатывает вложенные UObject* и TArray. Outer используется как владелец для иерархии объектов.

**Пины:**
- **Вход:** `Outer` (UObject*) — владелец для иерархии объектов, `InJsonString` (FString) — JSON-строка для десериализации
- **Выход:** `SpawnedObject` (UObject*) — созданный объект, `Success` (bool) — успешность операции

---

## Важно: Первичная компиляция

Это **C++ плагин**. Перед тем как использовать его в Blueprint-проекте, его **нужно один раз скомпилировать** в C++ проекте. Вот пошаговая инструкция:

### Шаг 1. Создайте временный C++ проект

1. Откройте **Unreal Engine 5.8** → нажмите **New Project**
2. Выберите категорию **Games** → выберите **Blank**
3. В настройках проекта выберите **C++** (НЕ Blueprint)
4. Назовите проект (например, `TempBuildProject`) и нажмите **Create**

> **Зачем?** Blueprint-проекты не имеют интеграции с Visual Studio и папки Source, поэтому C++ плагин в них скомпилировать нельзя. Нужен C++ проект, чтобы собрать DLL плагина.

### Шаг 2. Добавьте плагин в проект

1. Закройте Unreal Editor
2. Откройте папку проекта на диске
3. Создайте папку `Plugins` внутри проекта (если её нет)
4. Клонируйте этот репозиторий в папку `Plugins`:
   ```
   cd TempBuildProject/Plugins
   git clone https://github.com/Mmitekk/JsonObjectSerializer.git
   ```

### Шаг 3. Сгенерируйте файлы проекта

1. Правый клик на файле `.uproject` → **Generate Visual Studio project files**

### Шаг 4. Скомпилируйте плагин

1. Двойной клик на файле `.sln` → откроется **Visual Studio**
2. Установите конфигурацию решения **Development Editor** (верхняя панель)
3. В Solution Explorer правый клик по проекту → **Build**
4. Дождитесь окончания сборки — должно быть `Build: 1 succeeded, 0 failed`

> **Если сборка не удалась**, проверьте:
> - Версия UE — должна быть 5.8
> - В Visual Studio установлен компонент "Game development with C++"
> - Удалите папку `Intermediate` в корне проекта и попробуйте снова

### Шаг 5. Проверьте работу плагина

1. Откройте проект в **Unreal Editor**
2. Перейдите **Edit → Plugins** → найдите **Json Object Serializer** — должен быть включён
3. Откройте любой Blueprint → правый клик → ищите **Make Json From Object** и **Spawn Object From Json**
4. Если обе ноды появились — плагин работает

### Шаг 6. Скопируйте скомпилированный плагин в ваш Blueprint-проект

Теперь, когда плагин скомпилирован, его можно скопировать в любой проект (включая Blueprint-only):

1. Перейдите в папку скомпилированного проекта:
   ```
   TempBuildProject/Plugins/JsonObjectSerializer/
   ```
2. Скопируйте **папку** `JsonObjectSerializer` целиком в папку `Plugins` вашего целевого проекта:
   ```
   YourBlueprintProject/Plugins/JsonObjectSerializer/
   ```
3. Откройте ваш Blueprint-проект в Unreal Editor
4. Перейдите **Edit → Plugins** → включите **Json Object Serializer**
5. Ноды **Make Json From Object** и **Spawn Object From Json** теперь доступны

> **Важно:** Скомпилированные папки `Binaries` и `Intermediate` внутри плагина содержат DLL. Если вы скопируете только папку `Source` без `Binaries`, плагин не будет работать в Blueprint-проекте.

---

## Принцип работы

Плагин решает задачу сохранения данных вложенных `UObject` (например, объектов характеристик внутри `UActorComponent`), которые уничтожаются при завершении игры и не покрываются стандартной системой `SaveGame`.

### Сериализация — `Make Json From Object`

1. Перебирает все `UPROPERTY`-поля объекта (кроме помеченных `Transient`).
2. Определяет тип каждого поля через C++ Reflection (`CastField<FBoolProperty>`, `CastField<FIntProperty>` и т.д.).
3. Записывает имя переменной и её значение в JSON-объект.
4. При обнаружении `UObject*` — **рекурсивно** сериализует его как вложенный JSON-объект (с собственным `__ObjectClassPath`).
5. При обнаружении `TArray` — сериализует как JSON-массив, рекурсивно обрабатывая каждый элемент.
6. В корень JSON-объекта добавляется скрытое поле `__ObjectClassPath` со значением `GetClass()->GetPathName()` — путь к классу для последующей реконструкции.
7. Защита от бесконечной рекурсии: `TSet<const UObject*>` для обнаружения циклических ссылок и лимит глубины (32 уровня).

### Десериализация — `Spawn Object From Json`

1. Парсит JSON-строку через `TJsonReader` / `FJsonSerializer`.
2. Итерирует `FJsonObject::Values` для извлечения `__ObjectClassPath`, загружает класс через `LoadClass<UObject>`.
3. Создаёт экземпляр через `NewObject<UObject>(Outer, LoadedClass)`.
4. Строит карту свойств класса (`TMap<FName, FProperty*>`) и итерирует JSON-значения, мэтча ключи со свойствами по имени.
5. При обнаружении `UObject*`-свойства с вложенным JSON-объектом — **рекурсивно** вызывает создание вложенного объекта, передавая текущий объект как `Outer`.
6. При обнаружении `TArray` — очищает массив и заполняет его элементами из JSON-массива через `FScriptArrayHelper`.

---

## Пример использования

### Сохранение (Save Workflow)

```
┌──────────────────┐      ┌──────────────────────┐      ┌────────────────────┐
│  MyStatsObject   │ ───> │  Make Json From Object│ ───> │  JsonString        │
│  (UCharacterStats)│      │                      │      │  (FString)         │
└──────────────────┘      └──────────────────────┘      └────────┬───────────┘
                                                                  │
                                                                  v
                                                        ┌────────────────────┐
                                                        │  SaveGame Array    │
                                                        │  (Array of Strings)│
                                                        └────────────────────┘
```

1. Вызовите **Make Json From Object**, передав ваш `UObject` (например, объект характеристик).
2. Получите `JsonString` на выходе.
3. Сохраните строку в `SaveGame` (как элемент массива `FString` или одиночную строку).
4. Запишите `SaveGame` в слот через **Save Game to Slot**.

### Загрузка (Load Workflow)

```
┌────────────────┐      ┌──────────────────────┐      ┌──────────────────┐
│  SaveGame Slot │ ───> │  JsonString          │ ───> │ Spawn Object     │
│                │      │  (FString)           │      │  From Json       │
└────────────────┘      └──────────────────────┘      │  Outer:          │
                                                       │  MyComponent     │
                                                       └───────┬──────────┘
                                                               │
                                                               v
                                                       ┌──────────────────┐
                                                       │  SpawnedObject   │
                                                       │  (UCharacterStats)│
                                                       └──────────────────┘
```

1. Загрузите `SaveGame` из слота через **Load Game from Slot**.
2. Извлеките JSON-строку из массива `SaveGame`.
3. Вызовите **Spawn Object From Json**, передав `Outer` (например, ваш `UActorComponent`) и `JsonString`.
4. Получите восстановленный `UObject*` со всеми вложенными объектами, правильно привязанными по иерархии `Outer`.

### Пример с вложенными объектами

Создайте три Blueprint класса (родитель: Object):

- **BP_StatEntry** с переменными: `StatName` (String), `StatValue` (Float)
- **BP_CharacterStats** с переменными: `Level` (Integer), `Health` (Float), `Entries` (Array of BP_StatEntry*)
- **BP_StatsComponent** (ActorComponent) с переменной: `Stats` (BP_CharacterStats*)

**Сериализация** `Stats` через *Make Json From Object* создаст JSON:

```json
{
  "__ObjectClassPath": "/Script/MyGame.BP_CharacterStats_C",
  "Level": 5,
  "Health": 100.0,
  "Entries": [
    {
      "__ObjectClassPath": "/Script/MyGame.BP_StatEntry_C",
      "StatName": "Strength",
      "StatValue": 15.0
    },
    {
      "__ObjectClassPath": "/Script/MyGame.BP_StatEntry_C",
      "StatName": "Agility",
      "StatValue": 12.0
    }
  ]
}
```

**Десериализация** через *Spawn Object From Json* с `Outer = BP_StatsComponent` создаст:

- `BP_CharacterStats` (Outer = `BP_StatsComponent`)
- `BP_StatEntry` «Strength» (Outer = `BP_CharacterStats`)
- `BP_StatEntry` «Agility» (Outer = `BP_CharacterStats`)

---

## Поддерживаемые типы данных

| Тип C++          | Тип свойства UE      | JSON-представление |
|------------------|----------------------|--------------------|
| `bool`           | `FBoolProperty`      | Boolean            |
| `int32`          | `FIntProperty`       | Number             |
| `int64`          | `FInt64Property`     | Number             |
| `uint8`          | `FByteProperty`      | Number             |
| `uint16`         | `FNumericProperty`   | Number             |
| `uint32`         | `FNumericProperty`   | Number             |
| `uint64`         | `FNumericProperty`   | Number*            |
| `float`          | `FFloatProperty`     | Number             |
| `double`         | `FDoubleProperty`    | Number             |
| `FString`        | `FStrProperty`       | String             |
| `FName`          | `FNameProperty`      | String             |
| `FText`          | `FTextProperty`      | String**           |
| `enum` (uint8)   | `FByteProperty`      | Number             |
| `enum` (int+)    | `FEnumProperty`      | Number             |
| `UObject*`       | `FObjectProperty`    | Object (рекурсивно)|
| `TArray<T>`      | `FArrayProperty`     | Array (рекурсивно) |

> \* `uint16`, `uint32`, `uint64` обрабатываются через `FNumericProperty` (т.к. `FUInt16Property`, `FUInt32Property`, `FUInt64Property` были удалены в UE 5.4+). Значения сериализуются как знаковое целое через `GetSignedIntPropertyValue()`, что корректно для значений до 2^53 в JSON. Для `uint64` значений больше 2^53 возможна потеря точности.
>
> \*\* `FText` сериализуется через `ToString()` и восстанавливается через `FText::FromString()`. Ключ локализации и пространство имён теряются.

---

## ОГРАНИЧЕНИЯ: НЕСОХРАНЯЕМЫЕ ТИПЫ ДАННЫХ

Следующие типы данных **НЕ МОГУТ** быть корректно сериализованы и десериализованы данным плагином. При их обнаружении свойство пропускается.

### Указатели на Actors и внешние Components

**Почему:** `AActor` создаётся через `AActor::SpawnActor()`, а не через `NewObject()`. Плагин использует `NewObject` для создания объектов, что невозможно для Actor. Кроме того, плагин явно отклоняет классы-наследники `AActor` при десериализации. Внешний `UActorComponent`, принадлежащий другому Actor, также недоступен после перезапуска: его жизненный цикл привязан к Owner-Actor, который может быть ещё не создан.

**Решение:** Используйте идентификаторы (ID, Tag, GUID) для ссылки на Actors и Components. Сохраняйте ID как `FString` или `int32`, а при загрузке ищите объект по ID через `UGameplayStatics::GetAllActorsWithTag`, `FindActorByName` или кастомную систему реестра.

### Ассеты (UTexture2D*, UMaterialInterface*, USkeletalMesh*, USoundWave* и т.д.)

**Почему:** Ассеты — это тяжёлые ресурсы, загружаемые из дисковых пакетов (`.uasset`). Их содержимое (пиксели текстур, вершины мешей, аудиоданные) невозможно и бессмысленно упаковывать в JSON: одна текстура 1024x1024 — это мегабайты данных. Кроме того, ассеты управляются `FStreamableManager` и имеют особый жизненный цикл, несовместимый с простым `NewObject`.

**Решение:** Используйте `TSoftObjectPtr<T>` или `FString` с путём к ассету (например, `"/Game/Textures/T_PlayerSkin"`). При загрузке используйте `TSoftObjectPtr::LoadSynchronous()` или асинхронную загрузку через `FStreamableManager`. Плагин не сериализует `TSoftObjectPtr` автоматически, но вы можете добавить `FString`-поле с путём и преобразовывать его в коде.

### DataAssets (UDataAsset*)

**Почему:** `UDataAsset` — это статичный контент редактора, создаваемый на этапе разработки. Его данные неизменны в рантайме (за исключением редких случаев). Сериализация всего содержимого DataAsset в JSON дублирует данные, которые уже есть на диске в `.uasset`-файле. При загрузке нужно было бы пересоздать DataAsset через `NewObject`, но UE ожидает, что DataAsset загружается из пакета, а не создаётся динамически.

**Решение:** Сохраняйте только ссылку (путь) на DataAsset как `FSoftObjectPtr` или `FString`. При загрузке используйте `LoadObject<UDataAsset>()` или `TSoftObjectPtr` для получения существующего экземпляра.

### TMap и TSet

**Почему:** Unreal Engine Reflection не предоставляет стандартизированного API для итерации и модификации `TMap` и `TSet` аналогично `TArray` (где есть `FScriptArrayHelper`). Хотя `FMapProperty` и `FSetProperty` существуют в Reflection, они не имеют публичных хелперов уровня `FScriptArrayHelper`, которые позволяли бы безопасно перебирать и добавлять элементы без знания внутренней структуры хеш-таблицы.

**Решение:**
- **Для TMap:** Используйте массив `UObject*`, где каждый `UObject` содержит пару ключ-значение (например, `BP_MapEntry` с переменными `Key` и `Value`). Альтернативно — два парных массива: `TArray<FString> Keys` + `TArray<float> Values`.
- **Для TSet:** Используйте `TArray<T>` — уникальность элементов можно обеспечить логически.

### Делегаты (Delegates)

**Почему:** Делегаты привязаны к конкретным функциям и объектам в памяти (указатели на функции-члены, `UObject*` self). После перезапуска игры адреса объектов и функции могут измениться. Сериализация делегата — это попытка сохранить указатель на функцию в памяти, что не имеет смысла после перезапуска процесса.

**Решение:** Не сохраняйте делегаты. При загрузке пересоздавайте привязки программно (в `BeginPlay`, `OnConstruct` или после десериализации).

### Структуры (FStruct / UStruct)

**Почему:** Плагин создан специально для `UObject` — объектов с собственным жизненным циклом, иерархией `Outer` и возможностью создания через `NewObject`. `FStruct` (UStruct-структуры) — это типы значений (value types), встроенные непосредственно в память владельца. Для сериализации структур в UE5 уже существует стандартный инструмент — `FJsonObjectConverter`, который корректно обрабатывает все типы полей структур. Добавление поддержки `FStruct` в данный плагин привело бы к дублированию логики и усложнению кода без добавления новой функциональности.

**Решение:** Используйте `FJsonObjectConverter` для сериализации структур, либо оберните структуру в `UObject`-обёртку (добавьте `UPROPERTY() UMyWrapper*` вместо `UPROPERTY() FMyStruct`).

---

## Совместимость с UE 5.8

Плагин адаптирован под изменения API в **Unreal Engine 5.8**:

- **Удалённые типы свойств:** `FUInt16Property`, `FUInt32Property`, `FUInt64Property` были удалены в UE 5.4+. Беззнаковые целые обрабатываются через `FNumericProperty::IsInteger()` + `GetSignedIntPropertyValue()` / `SetIntPropertyValue()`.
- **Удалённые методы:** `FNumericProperty::IsUnsignedInt()` и `FNumericProperty::SetUnsignedIntPropertyValue()` удалены. Вместо них используется `SetIntPropertyValue()` для всех целочисленных типов (знаковых и беззнаковых — битовое представление совпадает).
- **Изменённый API FJsonObject:** Методы `GetField()`, `HasField()`, `TryGetStringField()` изменили сигнатуры (переход на `FStringView` и `UE::FSharedString` в качестве ключей `Values`). Десериализация итерирует `FJsonObject::Values` напрямую и строит карту свойств `TMap<FName, FProperty*>` для мэтча, полностью обходя проблемы совместимости.
- **Удалённый заголовок:** `UObject/NumericProperty.h` больше не существует — `FNumericProperty` определяется в `UObject/UnrealType.h`.

---

## Структура файлов плагина

```
JsonObjectSerializer/
├── JsonObjectSerializer.uplugin
├── README.md
└── Source/
    └── JsonObjectSerializerPlugin/
        ├── JsonObjectSerializerPlugin.Build.cs
        ├── Public/
        │   ├── JsonObjectSerializerPlugin.h
        │   └── JsonObjectSerializerBPLibrary.h
        └── Private/
            ├── JsonObjectSerializerPlugin.cpp
            └── JsonObjectSerializerBPLibrary.cpp
```

> **Примечание:** Модуль плагина называется `JsonObjectSerializerPlugin` (не `JsonObjectSerializer`), чтобы избежать коллизии имён с классом Build.cs проекта.

---

## Технические детали

| Параметр                      | Значение                                    |
|-------------------------------|---------------------------------------------|
| Целевая версия UE             | 5.8                                         |
| Тип модуля                    | Runtime                                     |
| Зависимости                   | `Json`, `JsonUtilities`, `CoreUObject`      |
| Максимальная глубина рекурсии | 32                                          |
| Защита от циклических ссылок  | `TSet<const UObject*>` при сериализации     |
| Формат JSON                   | Compact (без пробелов, минимальный размер)  |
| Создание объектов             | `NewObject<UObject>(Outer, LoadedClass)`    |
| Загрузка классов              | `LoadClass<UObject>(nullptr, ClassPath)`    |
| Доступ к JSON-полям           | Итерация `FJsonObject::Values` (UE 5.8)    |
