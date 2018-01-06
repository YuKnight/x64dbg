#include "module.h"
#include "TitanEngine/TitanEngine.h"
#include "threading.h"
#include "symbolinfo.h"
#include "murmurhash.h"
#include "symbolsourcepdb.h"
#include "memory.h"
#include "label.h"
#include <algorithm>
#include "console.h"

std::map<Range, MODINFO, RangeCompare> modinfo;
std::unordered_map<duint, std::string> hashNameMap;

bool MODRELOCATIONINFO::Contains(duint Address) const
{
    return Address >= rva && Address < rva + size;
}

static void ReadTlsCallbacks(MODINFO & Info, ULONG_PTR FileMapVA)
{
    // TODO: proper bounds checking

    // Clear TLS callbacks
    Info.tlsCallbacks.clear();

    // Get address and size of base relocation table
    duint tlsDirRva = GetPE32DataFromMappedFile(FileMapVA, 0, UE_TLSTABLEADDRESS);
    duint tlsDirSize = GetPE32DataFromMappedFile(FileMapVA, 0, UE_TLSTABLESIZE);
    if(tlsDirRva == 0 || tlsDirSize == 0)
        return;

    auto tlsDir = PIMAGE_TLS_DIRECTORY(ConvertVAtoFileOffsetEx(FileMapVA, Info.loadedSize, 0, tlsDirRva, true, false) + FileMapVA);
    if(!tlsDir || !tlsDir->AddressOfCallBacks)
        return;

    auto imageBase = GetPE32DataFromMappedFile(FileMapVA, 0, UE_IMAGEBASE);
    auto tlsArray = PULONG_PTR(ConvertVAtoFileOffsetEx(FileMapVA, Info.loadedSize, 0, tlsDir->AddressOfCallBacks - imageBase, true, false) + FileMapVA);
    if(!tlsArray)
        return;

    while(*tlsArray)
        Info.tlsCallbacks.push_back(*tlsArray++ - imageBase + Info.base);
}

static void ReadBaseRelocationTable(MODINFO & Info, ULONG_PTR FileMapVA)
{
    // Clear relocations
    Info.relocations.clear();

    // Parse base relocation table
    duint characteristics = GetPE32DataFromMappedFile(FileMapVA, 0, UE_CHARACTERISTICS);
    if((characteristics & IMAGE_FILE_RELOCS_STRIPPED) == IMAGE_FILE_RELOCS_STRIPPED)
        return;

    // Get address and size of base relocation table
    duint relocDirRva = GetPE32DataFromMappedFile(FileMapVA, 0, UE_RELOCATIONTABLEADDRESS);
    duint relocDirSize = GetPE32DataFromMappedFile(FileMapVA, 0, UE_RELOCATIONTABLESIZE);
    if(relocDirRva == 0 || relocDirSize == 0)
        return;

    auto relocDirOffset = (duint)ConvertVAtoFileOffsetEx(FileMapVA, Info.loadedSize, 0, relocDirRva, true, false);
    if(!relocDirOffset || relocDirOffset + relocDirSize > Info.loadedSize)
    {
        dprintf(QT_TRANSLATE_NOOP("DBG", "Invalid relocation directory for module %s%s...\n"), Info.name, Info.extension);
        return;
    }

    auto read = [&](duint offset, void* dest, size_t size)
    {
        if(offset + size > Info.loadedSize)
            return false;
        memcpy(dest, (char*)FileMapVA + offset, size);
        return true;
    };

    duint curPos = relocDirOffset;
    // Until we reach the end of base relocation table
    while(curPos < relocDirOffset + relocDirSize)
    {
        // Read base relocation block header
        IMAGE_BASE_RELOCATION baseRelocBlock;
        if(!read(curPos, &baseRelocBlock, sizeof(baseRelocBlock)))
        {
            dprintf(QT_TRANSLATE_NOOP("DBG", "Invalid relocation block for module %s%s...\n"), Info.name, Info.extension);
            return;
        }

        // For every entry in base relocation block
        duint count = (baseRelocBlock.SizeOfBlock - 8) / 2;
        for(duint i = 0; i < count; i++)
        {
            uint16 data = 0;
            if(!read(curPos + 8 + 2 * i, &data, sizeof(uint16)))
            {
                dprintf(QT_TRANSLATE_NOOP("DBG", "Invalid relocation entry for module %s%s...\n"), Info.name, Info.extension);
                return;
            }

            auto type = (data & 0xF000) >> 12;
            auto offset = data & 0x0FFF;

            switch(type)
            {
            case IMAGE_REL_BASED_HIGHLOW:
                Info.relocations.push_back(MODRELOCATIONINFO{ baseRelocBlock.VirtualAddress + offset, (BYTE)type, 4 });
                break;
            case IMAGE_REL_BASED_DIR64:
                Info.relocations.push_back(MODRELOCATIONINFO{ baseRelocBlock.VirtualAddress + offset, (BYTE)type, 8 });
                break;
            case IMAGE_REL_BASED_HIGH:
            case IMAGE_REL_BASED_LOW:
            case IMAGE_REL_BASED_HIGHADJ:
                Info.relocations.push_back(MODRELOCATIONINFO{ baseRelocBlock.VirtualAddress + offset, (BYTE)type, 2 });
                break;
            case IMAGE_REL_BASED_ABSOLUTE:
            default:
                break;
            }
        }

        curPos += baseRelocBlock.SizeOfBlock;
    }

    std::sort(Info.relocations.begin(), Info.relocations.end(), [](MODRELOCATIONINFO const & a, MODRELOCATIONINFO const & b)
    {
        return a.rva < b.rva;
    });
}

//Useful information: http://www.debuginfo.com/articles/debuginfomatch.html
void ReadDebugDirectory(MODINFO & Info, ULONG_PTR FileMapVA)
{
    //TODO: proper error handling and proper bounds checking
    auto pnth = PIMAGE_NT_HEADERS(FileMapVA + GetPE32DataFromMappedFile(FileMapVA, 0, UE_PE_OFFSET));
    auto & debugDataDir = pnth->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];

    if(!debugDataDir.VirtualAddress || !debugDataDir.Size)
        return;

    auto debugDirOffset = (duint)ConvertVAtoFileOffsetEx(FileMapVA, Info.loadedSize, 0, debugDataDir.VirtualAddress, true, false);
    if(!debugDirOffset || debugDirOffset + debugDataDir.Size > Info.loadedSize)
    {
        dprintf(QT_TRANSLATE_NOOP("DBG", "Invalid debug directory for module %s%s...\n"), Info.name, Info.extension);
        return;
    }

    IMAGE_DEBUG_DIRECTORY debugDir = { 0 };
    memcpy(&debugDir, (char*)FileMapVA + debugDirOffset, sizeof(debugDir));

    auto typeName = [](DWORD type)
    {
        switch(type)
        {
        case IMAGE_DEBUG_TYPE_UNKNOWN:
            return "IMAGE_DEBUG_TYPE_UNKNOWN";
        case IMAGE_DEBUG_TYPE_COFF:
            return "IMAGE_DEBUG_TYPE_COFF";
        case IMAGE_DEBUG_TYPE_CODEVIEW:
            return "IMAGE_DEBUG_TYPE_CODEVIEW";
        case IMAGE_DEBUG_TYPE_FPO:
            return "IMAGE_DEBUG_TYPE_FPO";
        case IMAGE_DEBUG_TYPE_MISC:
            return "IMAGE_DEBUG_TYPE_MISC";
        case IMAGE_DEBUG_TYPE_EXCEPTION:
            return "IMAGE_DEBUG_TYPE_EXCEPTION";
        case IMAGE_DEBUG_TYPE_FIXUP:
            return "IMAGE_DEBUG_TYPE_FIXUP";
        case IMAGE_DEBUG_TYPE_OMAP_TO_SRC:
            return "IMAGE_DEBUG_TYPE_OMAP_TO_SRC";
        case IMAGE_DEBUG_TYPE_OMAP_FROM_SRC:
            return "IMAGE_DEBUG_TYPE_OMAP_FROM_SRC";
        case IMAGE_DEBUG_TYPE_BORLAND:
            return "IMAGE_DEBUG_TYPE_BORLAND";
        case IMAGE_DEBUG_TYPE_RESERVED10:
            return "IMAGE_DEBUG_TYPE_RESERVED10";
        case IMAGE_DEBUG_TYPE_CLSID:
            return "IMAGE_DEBUG_TYPE_CLSID";
        default:
            return "unknown";
        }
    }(debugDir.Type);

    dprintf("IMAGE_DEBUG_DIRECTORY:\nCharacteristics: %08X\nTimeDateStamp: %08X\nMajorVersion: %04X\nMinorVersion: %04X\nType: %s\nSizeOfData: %08X\nAddressOfRawData: %08X\nPointerToRawData: %08X\n",
            debugDir.Characteristics, debugDir.TimeDateStamp, debugDir.MajorVersion, debugDir.MinorVersion, typeName, debugDir.SizeOfData, debugDir.AddressOfRawData, debugDir.PointerToRawData);

    if(debugDir.Type != IMAGE_DEBUG_TYPE_CODEVIEW) //TODO: support other types (DBG)?
    {
        dprintf(QT_TRANSLATE_NOOP("DBG", "Unsupported debug type %s in module %s%s...\n"), typeName, Info.name, Info.extension);
        return;
    }

    struct CV_HEADER
    {
        DWORD Signature;
        DWORD Offset;
    };

    struct CV_INFO_PDB20
    {
        CV_HEADER CvHeader; //CvHeader.Signature = "NB10"
        DWORD Signature;
        DWORD Age;
        BYTE PdbFileName[1];
    };

    struct CV_INFO_PDB70
    {
        DWORD CvSignature; //"RSDS"
        GUID Signature;
        DWORD Age;
        BYTE PdbFileName[1];
    };

    auto codeViewOffset = (duint)ConvertVAtoFileOffsetEx(FileMapVA, Info.loadedSize, 0, debugDir.AddressOfRawData, true, false);;
    if(!codeViewOffset)
    {
        dprintf(QT_TRANSLATE_NOOP("DBG", "Invalid debug directory for module %s%s...\n"), Info.name, Info.extension);
        return;
    }

    //TODO: bounds checking with debugDir.SizeOfData

    auto signature = *(DWORD*)(FileMapVA + codeViewOffset);
    if(signature == '01BN')
    {
        auto cv = (CV_INFO_PDB20*)(FileMapVA + codeViewOffset);
        Info.pdbSignature = StringUtils::sprintf("%X%X", cv->Signature, cv->Age);
        Info.pdbFile = String((const char*)cv->PdbFileName);
    }
    else if(signature == 'SDSR')
    {
        auto cv = (CV_INFO_PDB70*)(FileMapVA + codeViewOffset);
        Info.pdbSignature = StringUtils::sprintf("%08X%04X%04X%s%X",
                            cv->Signature.Data1, cv->Signature.Data2, cv->Signature.Data3,
                            StringUtils::ToHex(cv->Signature.Data4, 8).c_str(),
                            cv->Age);
        Info.pdbFile = String((const char*)cv->PdbFileName);
    }
    else
    {
        dprintf(QT_TRANSLATE_NOOP("DBG", "Unknown debug directory signature %08X for module %s%s...\n"), signature, Info.name, Info.extension);
        return;
    }

    dprintf("%s%s pdbSignature: %s, pdbFile: \"%s\"\n", Info.name, Info.extension, Info.pdbSignature.c_str(), Info.pdbFile.c_str());
}

void GetModuleInfo(MODINFO & Info, ULONG_PTR FileMapVA)
{
    // Get the entry point
    duint moduleOEP = GetPE32DataFromMappedFile(FileMapVA, 0, UE_OEP);

    // Fix a problem where the OEP is set to zero (non-existent).
    // OEP can't start at the PE header/offset 0 -- except if module is an EXE.
    Info.entry = moduleOEP + Info.base;

    if(!moduleOEP)
    {
        WORD characteristics = (WORD)GetPE32DataFromMappedFile(FileMapVA, 0, UE_CHARACTERISTICS);

        // If this wasn't an exe, invalidate the entry point
        if((characteristics & IMAGE_FILE_DLL) == IMAGE_FILE_DLL)
            Info.entry = 0;
    }

    // Enumerate all PE sections
    Info.sections.clear();
    int sectionCount = (int)GetPE32DataFromMappedFile(FileMapVA, 0, UE_SECTIONNUMBER);

    for(int i = 0; i < sectionCount; i++)
    {
        MODSECTIONINFO curSection;
        memset(&curSection, 0, sizeof(MODSECTIONINFO));

        curSection.addr = GetPE32DataFromMappedFile(FileMapVA, i, UE_SECTIONVIRTUALOFFSET) + Info.base;
        curSection.size = GetPE32DataFromMappedFile(FileMapVA, i, UE_SECTIONVIRTUALSIZE);
        const char* sectionName = (const char*)GetPE32DataFromMappedFile(FileMapVA, i, UE_SECTIONNAME);

        // Escape section name when needed
        strcpy_s(curSection.name, StringUtils::Escape(sectionName).c_str());

        // Add entry to the vector
        Info.sections.push_back(curSection);
    }

    // Clear imports by default
    Info.imports.clear();

    ReadTlsCallbacks(Info, FileMapVA);
    ReadBaseRelocationTable(Info, FileMapVA);
    ReadDebugDirectory(Info, FileMapVA);
}

bool ModLoad(duint Base, duint Size, const char* FullPath)
{
    // Handle a new module being loaded
    if(!Base || !Size || !FullPath)
        return false;

    // Copy the module path in the struct
    MODINFO info;
    strcpy_s(info.path, FullPath);

    // Break the module path into a directory and file name
    char file[MAX_MODULE_SIZE];
    {
        char dir[MAX_PATH];
        memset(dir, 0, sizeof(dir));

        // Dir <- lowercase(file path)
        strcpy_s(dir, FullPath);
        _strlwr_s(dir);

        // Find the last instance of a path delimiter (slash)
        char* fileStart = strrchr(dir, '\\');

        if(fileStart)
        {
            strcpy_s(file, fileStart + 1);
            fileStart[0] = '\0';
        }
        else
            strcpy_s(file, FullPath);
    }

    // Calculate module hash from full file name
    info.hash = ModHashFromName(file);

    // Copy the extension into the module struct
    {
        char* extensionPos = strrchr(file, '.');

        if(extensionPos)
        {
            strcpy_s(info.extension, extensionPos);
            extensionPos[0] = '\0';
        }
    }

    // Copy information to struct
    strcpy_s(info.name, file);
    info.base = Base;
    info.size = Size;
    info.fileHandle = nullptr;
    info.loadedSize = 0;
    info.fileMap = nullptr;
    info.fileMapVA = 0;
    //info.invalidSymbols.resize(Size);

    // Determine whether the module is located in system
    wchar_t sysdir[MAX_PATH];
    GetEnvironmentVariableW(L"windir", sysdir, _countof(sysdir));
    String Utf8Sysdir = StringUtils::Utf16ToUtf8(sysdir);
    Utf8Sysdir.append("\\");
    if(_memicmp(Utf8Sysdir.c_str(), FullPath, Utf8Sysdir.size()) == 0)
    {
        info.party = 1;
    }
    else
    {
        info.party = 0;
    }

    // Load module data
    bool virtualModule = strstr(FullPath, "virtual:\\") == FullPath;

    if(!virtualModule)
    {
        auto wszFullPath = StringUtils::Utf8ToUtf16(FullPath);

        // Load the physical module from disk
        if(StaticFileLoadW(wszFullPath.c_str(), UE_ACCESS_READ, false, &info.fileHandle, &info.loadedSize, &info.fileMap, &info.fileMapVA))
        {
            GetModuleInfo(info, info.fileMapVA);

            Size = GetPE32DataFromMappedFile(info.fileMapVA, 0, UE_SIZEOFIMAGE);
            info.size = Size;
        }
        else
        {
            info.fileHandle = nullptr;
            info.loadedSize = 0;
            info.fileMap = nullptr;
            info.fileMapVA = 0;
        }
    }
    else
    {
        // This was a virtual module -> read it remotely
        Memory<unsigned char*> data(Size);
        MemRead(Base, data(), data.size());

        // Get information from the local buffer
        // TODO: this does not properly work for file offset -> rva conversions (since virtual modules are SEC_IMAGE)
        GetModuleInfo(info, (ULONG_PTR)data());
    }

    // Load Symbols.
    info.symbols = &EmptySymbolSource; // Set to empty as default one.

    // Try DIA
    if(info.symbols == &EmptySymbolSource &&
            SymbolSourcePDB::isLibraryAvailable())
    {
        SymbolSourcePDB* symSource = new SymbolSourcePDB();
        if(symSource->loadPDB(info.path, Base, info.size))
        {
            symSource->resizeSymbolBitmap(info.size);

            info.symbols = symSource;

            std::string msg;
            if(symSource->isLoading())
                msg = StringUtils::sprintf("Loading async (MSDIA) PDB: %s\n", info.path);
            else
                msg = StringUtils::sprintf("Loaded (MSDIA) PDB: %s\n", info.path);

            GuiAddLogMessage(msg.c_str());
        }
        else
        {
            delete symSource;
        }
    }
    if(info.symbols == &EmptySymbolSource &&
            true /* TODO */)
    {
    }

    if(info.symbols->isOpen() == false)
    {
        std::string msg = StringUtils::sprintf("No symbols loaded for: %s\n", info.path);
        GuiAddLogMessage(msg.c_str());
    }

    // Add module to list
    EXCLUSIVE_ACQUIRE(LockModules);
    modinfo.insert(std::make_pair(Range(Base, Base + Size - 1), info));
    EXCLUSIVE_RELEASE();

    // Put labels for virtual module exports
    if(virtualModule)
    {
        if(info.entry >= Base && info.entry < Base + Size)
            LabelSet(info.entry, "EntryPoint", false);

        apienumexports(Base, [](duint base, const char* mod, const char* name, duint addr)
        {
            LabelSet(addr, name, false);
        });
    }

    SymUpdateModuleList();
    return true;
}

bool ModUnload(duint Base)
{
    EXCLUSIVE_ACQUIRE(LockModules);

    // Find the iterator index
    const auto found = modinfo.find(Range(Base, Base));

    if(found == modinfo.end())
        return false;

    // Unload the mapped file from memory
    const auto & info = found->second;
    if(info.fileMapVA)
        StaticFileUnloadW(StringUtils::Utf8ToUtf16(info.path).c_str(), false, info.fileHandle, info.loadedSize, info.fileMap, info.fileMapVA);

    if(info.symbols != nullptr &&
            info.symbols != &EmptySymbolSource)
    {
        if(info.symbols->isLoading())
        {
            info.symbols->cancelLoading();
        }

        delete info.symbols;
    }

    // Remove it from the list
    modinfo.erase(found);
    EXCLUSIVE_RELEASE();

    // Update symbols
    SymUpdateModuleList();
    return true;
}

void ModClear()
{
    {
        // Clean up all the modules
        EXCLUSIVE_ACQUIRE(LockModules);

        for(const auto & mod : modinfo)
        {
            // Unload the mapped file from memory
            const auto & info = mod.second;
            if(info.fileMapVA)
                StaticFileUnloadW(StringUtils::Utf8ToUtf16(info.path).c_str(), false, info.fileHandle, info.loadedSize, info.fileMap, info.fileMapVA);

            if(info.symbols != nullptr &&
                    info.symbols != &EmptySymbolSource)
            {
                if(info.symbols->isLoading())
                {
                    info.symbols->cancelLoading();
                }

                delete info.symbols;
            }
        }

        modinfo.clear();
    }

    {
        // Clean up the reverse hash map
        EXCLUSIVE_ACQUIRE(LockModuleHashes);
        hashNameMap.clear();
    }

    // Tell the symbol updater
    GuiSymbolUpdateModuleList(0, nullptr);
}

MODINFO* ModInfoFromAddr(duint Address)
{
    //
    // NOTE: THIS DOES _NOT_ USE LOCKS
    //
    auto found = modinfo.find(Range(Address, Address));

    // Was the module found with this address?
    if(found == modinfo.end())
        return nullptr;

    return &found->second;
}

bool ModNameFromAddr(duint Address, char* Name, bool Extension)
{
    ASSERT_NONNULL(Name);
    SHARED_ACQUIRE(LockModules);

    // Get a pointer to module information
    auto module = ModInfoFromAddr(Address);

    if(!module)
    {
        Name[0] = '\0';
        return false;
    }

    // Copy initial module name
    strcpy_s(Name, MAX_MODULE_SIZE, module->name);

    if(Extension)
        strcat_s(Name, MAX_MODULE_SIZE, module->extension);

    return true;
}

duint ModBaseFromAddr(duint Address)
{
    SHARED_ACQUIRE(LockModules);

    auto module = ModInfoFromAddr(Address);

    if(!module)
        return 0;

    return module->base;
}

duint ModHashFromAddr(duint Address)
{
    // Returns a unique hash from a virtual address
    SHARED_ACQUIRE(LockModules);

    auto module = ModInfoFromAddr(Address);

    if(!module)
        return Address;

    return module->hash + (Address - module->base);
}

duint ModContentHashFromAddr(duint Address)
{
    SHARED_ACQUIRE(LockModules);

    auto module = ModInfoFromAddr(Address);

    if(!module)
        return 0;

    if(module->fileMapVA != 0 && module->loadedSize > 0)
        return murmurhash((void*)module->fileMapVA, module->loadedSize);
    else
        return 0;
}

duint ModHashFromName(const char* Module)
{
    // return MODINFO.hash (based on the name)
    ASSERT_NONNULL(Module);
    auto len = int(strlen(Module));
    if(!len)
        return 0;
    auto hash = murmurhash(Module, len);

    //update the hash cache
    SHARED_ACQUIRE(LockModuleHashes);
    auto hashInCache = hashNameMap.find(hash) != hashNameMap.end();
    SHARED_RELEASE();
    if(!hashInCache)
    {
        EXCLUSIVE_ACQUIRE(LockModuleHashes);
        hashNameMap[hash] = Module;
    }

    return hash;
}

duint ModBaseFromName(const char* Module)
{
    ASSERT_NONNULL(Module);
    auto len = int(strlen(Module));
    if(!len)
        return 0;
    ASSERT_TRUE(len < MAX_MODULE_SIZE);
    SHARED_ACQUIRE(LockModules);

    //TODO: refactor this to query from a map
    duint candidate = 0;
    for(const auto & i : modinfo)
    {
        const auto & currentModule = i.second;
        char currentModuleName[MAX_MODULE_SIZE];
        strcpy_s(currentModuleName, currentModule.name);
        strcat_s(currentModuleName, currentModule.extension);

        // Compare with extension (perfect match)
        if(!_stricmp(currentModuleName, Module))
            return currentModule.base;

        // Compare without extension, possible candidate (thanks to chessgod101 for finding this)
        if(!candidate && !_stricmp(currentModule.name, Module))
            candidate = currentModule.base;
    }

    return candidate;
}

duint ModSizeFromAddr(duint Address)
{
    SHARED_ACQUIRE(LockModules);

    auto module = ModInfoFromAddr(Address);

    if(!module)
        return 0;

    return module->size;
}

std::string ModNameFromHash(duint Hash)
{
    SHARED_ACQUIRE(LockModuleHashes);
    auto found = hashNameMap.find(Hash);
    if(found == hashNameMap.end())
        return std::string();
    return found->second;
}

bool ModSectionsFromAddr(duint Address, std::vector<MODSECTIONINFO>* Sections)
{
    SHARED_ACQUIRE(LockModules);

    auto module = ModInfoFromAddr(Address);

    if(!module)
        return false;

    // Copy vector <-> vector
    *Sections = module->sections;
    return true;
}

bool ModImportsFromAddr(duint Address, std::vector<MODIMPORTINFO>* Imports)
{
    SHARED_ACQUIRE(LockModules);

    auto module = ModInfoFromAddr(Address);

    if(!module)
        return false;

    // Copy vector <-> vector
    *Imports = module->imports;
    return true;
}

duint ModEntryFromAddr(duint Address)
{
    SHARED_ACQUIRE(LockModules);

    auto module = ModInfoFromAddr(Address);

    if(!module)
        return 0;

    return module->entry;
}

int ModPathFromAddr(duint Address, char* Path, int Size)
{
    SHARED_ACQUIRE(LockModules);

    auto module = ModInfoFromAddr(Address);

    if(!module)
        return 0;

    strcpy_s(Path, Size, module->path);
    return (int)strlen(Path);
}

int ModPathFromName(const char* Module, char* Path, int Size)
{
    return ModPathFromAddr(ModBaseFromName(Module), Path, Size);
}

void ModGetList(std::vector<MODINFO> & list)
{
    SHARED_ACQUIRE(LockModules);
    list.clear();
    list.reserve(modinfo.size());
    for(const auto & mod : modinfo)
        list.push_back(mod.second);
}

void ModEnum(const std::function<void(const MODINFO &)> & cbEnum)
{
    SHARED_ACQUIRE(LockModules);
    for(const auto & mod : modinfo)
        cbEnum(mod.second);
}

bool ModAddImportToModule(duint Base, const MODIMPORTINFO & importInfo)
{
    SHARED_ACQUIRE(LockModules);

    if(!Base || !importInfo.addr)
        return false;

    auto module = ModInfoFromAddr(Base);

    if(!module)
        return false;

    // Search in Import Vector
    auto pImports = &(module->imports);
    auto it = std::find_if(pImports->begin(), pImports->end(), [&importInfo](const MODIMPORTINFO & currentImportInfo)->bool
    {
        return (importInfo.addr == currentImportInfo.addr);
    });

    // Import in the list already
    if(it != pImports->end())
        return false;

    // Add import to imports vector
    pImports->push_back(importInfo);

    return true;
}

int ModGetParty(duint Address)
{
    SHARED_ACQUIRE(LockModules);

    auto module = ModInfoFromAddr(Address);

    // If the module is not found, it is an user module
    if(!module)
        return 0;

    return module->party;
}

void ModSetParty(duint Address, int Party)
{
    EXCLUSIVE_ACQUIRE(LockModules);

    auto module = ModInfoFromAddr(Address);

    // If the module is not found, it is an user module
    if(!module)
        return;

    module->party = Party;
}

bool ModRelocationsFromAddr(duint Address, std::vector<MODRELOCATIONINFO> & Relocations)
{
    SHARED_ACQUIRE(LockModules);

    auto module = ModInfoFromAddr(Address);

    if(!module || module->relocations.empty())
        return false;

    Relocations = module->relocations;

    return true;
}

bool ModRelocationAtAddr(duint Address, MODRELOCATIONINFO* Relocation)
{
    SHARED_ACQUIRE(LockModules);

    auto module = ModInfoFromAddr(Address);

    if(!module || module->relocations.empty())
        return false;

    DWORD rva = (DWORD)(Address - module->base);

    // We assume there are no overlapping relocations
    auto ub = std::upper_bound(module->relocations.cbegin(), module->relocations.cend(), rva,
                               [](DWORD a, MODRELOCATIONINFO const & b)
    {
        return a < b.rva;
    });
    if(ub != module->relocations.begin() && (--ub)->Contains(rva))
    {
        if(Relocation)
            *Relocation = *ub;
        return true;
    }

    return false;
}

bool ModRelocationsInRange(duint Address, duint Size, std::vector<MODRELOCATIONINFO> & Relocations)
{
    SHARED_ACQUIRE(LockModules);

    auto module = ModInfoFromAddr(Address);

    if(!module || module->relocations.empty())
        return false;

    DWORD rva = (DWORD)(Address - module->base);

    // We assume there are no overlapping relocations
    auto ub = std::upper_bound(module->relocations.cbegin(), module->relocations.cend(), rva,
                               [](DWORD a, MODRELOCATIONINFO const & b)
    {
        return a < b.rva;
    });
    if(ub != module->relocations.begin())
        ub--;

    Relocations.clear();
    while(ub != module->relocations.end() && ub->rva < rva + Size)
    {
        if(ub->rva >= rva)
            Relocations.push_back(*ub);
        ub++;
    }

    return !Relocations.empty();
}
