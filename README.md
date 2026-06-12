# JsonObjectSerializer

Плагин для **Unreal Engine 5**, предоставляющий Blueprint-ноды для глубокой сериализации `UObject` в JSON-строку и десериализации обратно. Использует **C++ Reflection** для перебора всех `UPROPERTY`-полей, включая рекурсивную обработку вложенных `UObject*` и `TArray`.

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
2. Извлекает `__ObjectClassPath`, загружает класс через `LoadClass<UObject>`.
3. Создаёт экземпляр через `NewObject<UObject>(Outer, LoadedClass)`.
4. Перебирает свойства созданного объекта, ищет совпадающие ключи в JSON и устанавливает значения через Reflection.
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

```cpp
UCLASS()
class UStatEntry : public UObject
{
    GENERATED_BODY()
public:
    UPROPERTY() FString StatName;
    UPROPERTY() float StatValue;
};

UCLASS()
class UCharacterStats : public UObject
{
    GENERATED_BODY()
public:
    UPROPERTY() int32 Level;
    UPROPERTY() float Health;
    UPROPERTY() TArray<UStatEntry*> Entries;
};

UCLASS()
class UStatsComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UPROPERTY() UCharacterStats* Stats;
};
```

**Сериализация** `Stats` через *Make Json From Object* создаст JSON:

```json
{
  "__ObjectClassPath": "/Script/MyGame.CharacterStats",
  "Level": 5,
  "Health": 100.0,
  "Entries": [
    {
      "__ObjectClassPath": "/Script/MyGame.StatEntry",
      "StatName": "Strength",
      "StatValue": 15.0
    },
    {
      "__ObjectClassPath": "/Script/MyGame.StatEntry",
      "StatName": "Agility",
      "StatValue": 12.0
    }
  ]
}
```

**Десериализация** через *Spawn Object From Json* с `Outer = UStatsComponent` создаст:

- `UCharacterStats` (Outer = `UStatsComponent`)
- `UStatEntry` «Strength» (Outer = `UCharacterStats`)
- `UStatEntry` «Agility» (Outer = `UCharacterStats`)

---

## Поддерживаемые типы данных

| Тип C++          | Тип свойства UE      | JSON-представление |
|------------------|----------------------|--------------------|
| `bool`           | `FBoolProperty`      | Boolean            |
| `int32`          | `FIntProperty`       | Number             |
| `int64`          | `FInt64Property`     | Number             |
| `uint8`          | `FByteProperty`      | Number             |
| `uint16`         | `FUInt16Property`    | Number             |
| `uint32`         | `FUInt32Property`    | Number             |
| `uint64`         | `FUInt64Property`    | Number*            |
| `float`          | `FFloatProperty`     | Number             |
| `double`         | `FDoubleProperty`    | Number             |
| `FString`        | `FStrProperty`       | String             |
| `FName`          | `FNameProperty`      | String             |
| `FText`          | `FTextProperty`      | String**           |
| `enum` (uint8)   | `FByteProperty`      | Number             |
| `enum` (int+)    | `FEnumProperty`      | Number             |
| `UObject*`       | `FObjectProperty`    | Object (рекурсивно)|
| `TArray<T>`      | `FArrayProperty`     | Array (рекурсивно) |

> \* `uint64` сериализуется как `double` в JSON. Для значений больше 2^53 возможна потеря точности.
>
> \*\* `FText` сериализуется через `ToString()` и восстанавливается через `FText::FromString()`. Ключ локализации и пространство имён теряются.

---

## ОГРАНИЧЕНИЯ: НЕСОХРАНЯЕМЫЕ ТИПЫ ДАННЫХ

Следующие типы данных **НЕ МОГУТ** быть корректно сериализованы и десериализованы данным плагином. При их обнаружении свойство пропускается (с логированием уровня Verbose).

### Указатели на Actors и внешние Components

**Почему:** `AActor` создаётся через `AActor::SpawnActor()`, а не через `NewObject()`. Плагин использует `NewObject` для создания объектов, что невозможно для Actor. Даже если бы сериализация удалась, после перезапуска игры экземпляр Actor с прежним адресом не существует — указатель протухнет. Внешний `UActorComponent`, принадлежащий другому Actor, также недоступен после перезапуска: его生命周期 привязан к Owner-Actor, который может быть ещё не создан.

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
- **Для TMap:** Используйте массив `UObject*`, где каждый `UObject` содержит пару ключ-значение (например, `UMapEntry` с полями `Key` и `Value`). Альтернативно — два парных массива: `TArray<FString> Keys` + `TArray<float> Values`.
- **Для TSet:** Используйте `TArray<T>` — уникальность элементов можно обеспечить логически.

### Делегаты (Delegates)

**Почему:** Делегаты привязаны к конкретным функциям и объекты в памяти (указатели на функции-члены, `UObject*` self). После перезапуска игры адреса объектов и функции могут измениться. Сериализация делегата — это попытка сохранить указатель на функцию в памяти, что не имеет смысла после перезапуска процесса.

**Решение:** Не сохраняйте делегаты. При загрузке пересоздавайте привязки программно (в `BeginPlay`, `OnConstruct` или после десериализации).

### Структуры (FStruct / UStruct)

**Почему:** Плагин создан специально для `UObject` — объектов с собственным жизненным циклом, иерархией `Outer` и возможностью создания через `NewObject`. `FStruct` (UStruct-структуры) — это типы значений (value types), встроенные непосредственно в память владельца. Для сериализации структур в UE5 уже существует стандартный инструмент — `FJsonObjectConverter`, который корректно обрабатывает все типы полей структур. Добавление поддержки `FStruct` в данный плагин привело бы к дублированию логики и усложнению кода без добавления новой функциональности.

**Решение:** Используйте `FJsonObjectConverter` для сериализации структур, либо оберните структуру в `UObject`-обёртку (добавьте `UPROPERTY() UMyWrapper*` вместо `UPROPERTY() FMyStruct`).

---

## Установка

1. Скопируйте папку `JsonObjectSerializer` в директорию `Plugins/` вашего проекта:
   ```
   MyProject/
   └── Plugins/
       └── JsonObjectSerializer/
           ├── JsonObjectSerializer.uplugin
           ├── Source/
           │   └── JsonObjectSerializer/
           │       ├── JsonObjectSerializer.Build.cs
           │       ├── Public/
           │       │   └── JsonObjectSerializerBPLibrary.h
           │       └── Private/
           │           └── JsonObjectSerializerBPLibrary.cpp
           └── README.md
   ```
2. Перегенерируйте файлы проекта: правый клик на `.uproject` → **Generate Visual Studio project files**.
3. Скомпилируйте проект.
4. Включите плагин: **Edit → Plugins → Json Object Serializer** (если не включен автоматически).

---

## Технические детали

| Параметр                      | Значение                                    |
|-------------------------------|---------------------------------------------|
| Тип модуля                    | Runtime                                     |
| Зависимости                   | `Json`, `JsonUtilities`, `CoreUObject`      |
| Максимальная глубина рекурсии | 32                                          |
| Защита от циклических ссылок  | `TSet<const UObject*>` при сериализации     |
| Формат JSON                   | Compact (без пробелов, минимальный размер)  |
| Создание объектов             | `NewObject<UObject>(Outer, LoadedClass)`    |
| Загрузка классов              | `LoadClass<UObject>(nullptr, ClassPath)`    |

### Логирование

Плагин использует категорию лога `LogJsonObjectSerializer`. Уровни:

- **Log** — старт/остановка модуля.
- **Verbose** — пропущенные свойства, отдельные элементы массивов.
- **Warning** — циклические ссылки, null-указатели, несоответствия типов.
- **Error** — критические ошибки (класс не найден, JSON невалиден, Actor вместо UObject).

Для включения Verbose-лога в консоли:
```
Log LogJsonObjectSerializer Verbose
```
