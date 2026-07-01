
#include "archive.h"

#define MINIZ_HEADER_FILE_ONLY
extern "C" {
#include <miniz/miniz.c>
}
//#include "ziplib/zip.h"

#include <vector>
#include <cstring>
#include <coreutils/log.h>
//#define _UNIX
#ifdef _WIN32
#include <windows.h>
#endif
#include  "unrar/dll.hpp"

using namespace std;

namespace utils {

/*
class ExtArchive : public Archive {
	File extract(const string &name) {
		system("lha x " + name);
	}
};*/

class ZipFile : public Archive {
public:
	ZipFile(const string &fileName, const string &workDir = ".") : workDir(workDir) {
		//zipFile = zip_open(fileName.c_str(), 0, NULL);
		memset(&zipArchive, 0, sizeof(zipArchive));
		if(!mz_zip_reader_init_file(&zipArchive, fileName.c_str(), 0))
			throw archive_exception("Could not open zip file");
	}

	~ZipFile() {
		close();
	}

	void close() {
		mz_zip_reader_end(&zipArchive);
	}

	File extract(const string &name) {

		File file(workDir + "/" + name);
		// A zip directory entry (name ends with '/') is not a file: extracting it
		// would drop a 0-byte FILE where a folder belongs, so a sibling member
		// like "dir/song.mp3" then fails to write ("Not a directory"). Skip it.
		if (!name.empty() && name.back() == '/')
			return file;
		// Members can be nested in folders (e.g. "Atari 2600 Music Compo/x.mp3").
		// mz_zip_reader_extract_file_to_file does NOT create intermediate dirs, so
		// make the parent first or the extract silently fails and the file never
		// appears at getName().
		auto parent = utils::path_directory(file.getName());
		// A previous (buggy) run that extracted the folder's own dir entry left a
		// 0-byte FILE where this parent directory belongs; drop it so makedirs can
		// create the real dir (otherwise mkdir keeps failing and the member never
		// extracts -> "Not a directory" at play time).
		if (!parent.empty() && utils::File::exists(parent) &&
		    !utils::File(parent).isDir())
			utils::File::remove(parent);
		utils::makedirs(parent);
		// NB: the 3rd arg is the destination FILE path, not a directory -- passing
		// workDir here wrote every member onto the dir itself (a no-op/failure), so
		// the extracted file never appeared at getName(). Use the full file path.
		mz_zip_reader_extract_file_to_file(&zipArchive, name.c_str(),
		                                   file.getName().c_str(), 0);
		return file;

		/*int i = zip_name_locate(zipFile, name.c_str(), ZIP_FL_NOCASE);
		if(i >= 0) {
			struct zip_file *zf = zip_fopen_index(zipFile, i, 0);
			File file(workDir + "/" + name);
			vector<uint8_t> buffer(2048);
			while(true) {
				int bytes = zip_fread(zf, &buffer[0], buffer.size());
				if(bytes > 0)
					file.write(&buffer[0], bytes);
				else
					break;
			}
			file.close();
			zip_fclose(zf);
			return file;
		}
		return File();*/
	}

	virtual string nameFromPosition(int pos) const {
	mz_zip_archive_file_stat file_stat;
    if(!mz_zip_reader_file_stat(const_cast<mz_zip_archive*>(&zipArchive), pos, &file_stat))
    {}
	return string(file_stat.m_filename);

		//struct zip_stat sz;
		//zip_stat_index(zipFile, pos, 0, &sz);
		//return string(sz.name);
	}

	virtual int totalFiles() const {
		return mz_zip_reader_get_num_files(const_cast<mz_zip_archive*>(&zipArchive));
		//return zip_get_num_files(zipFile);
	}

private:
	mz_zip_archive zipArchive;
	//struct zip *zipFile;
	string workDir;
};


class RarFile : public Archive {
public:
	RarFile(const string &fileName, const string &workDir = ".") : workDir(workDir) {
		//fprintf(stderr, "CONSTR");
		//fflush(stderr);
		RAROpenArchiveDataEx archiveInfo;
		memset(&archiveInfo, 0, sizeof(archiveInfo));
		archiveInfo.CmtBuf = NULL;
		archiveInfo.OpenMode = RAR_OM_EXTRACT;
		archiveInfo.ArcName = (char*)fileName.c_str();
		rarFile = RAROpenArchiveEx(&archiveInfo);
		if(archiveInfo.OpenResult != 0) {
			throw archive_exception((std::string("Bad RAR code ") + std::to_string(archiveInfo.OpenResult)).c_str());
		};
		currentPos = 0;
		RHCode = RARReadHeaderEx(rarFile, &fileInfo);


	}

	~RarFile() {
		//fprintf(stderr, "DESTR");
		//fflush(stderr);
		RARCloseArchive(rarFile);
	}

	File extract(const string &name) {
		//RARHeaderDataEx fileInfo;
		//int RHCode = RARReadHeaderEx(rarFile, &fileInfo);

		//int RHCode = RARReadHeaderEx(rarFile, &fileInfo);
		//LOGD("RHCode %d %s", RHCode, fileInfo.FileName);
		//if(RHCode !=0)
		//	return File();

		int PFCode = RARProcessFile(rarFile, RAR_EXTRACT, (char*)workDir.c_str(), NULL);

		//LOGD("extract %d", PFCode);

		RHCode = RARReadHeaderEx(rarFile, &fileInfo);

		currentPos++;

		File f { workDir + "/" + fileInfo.FileName };

		return f;
	}

	virtual string nameFromPosition(int pos) const {

		//LOGD("POS %d vs %d", pos , currentPos);
		while(currentPos < pos) {
			int PFCode = RARProcessFile(rarFile, RAR_SKIP, NULL, NULL);
			//LOGD("PFCode %d", PFCode);

			RHCode = RARReadHeaderEx(rarFile, &fileInfo);

			currentPos++;
		}

		if(RHCode != 0)
			return "";

		//int RHCode = RARReadHeaderEx(rarFile, &fileInfo);
		//LOGD("pos %d %s", currentPos, fileInfo.FileName);
		//if(RHCode !=0)
		//	return "";
		return fileInfo.FileName;
	}

	virtual int totalFiles() const {
		return -1;
	}

private:

	HANDLE rarFile;
	mutable int currentPos;
	//struct zip *zipFile;
	mutable RARHeaderDataEx fileInfo;
	mutable int RHCode;
	string workDir;
};


Archive *Archive::open(const std::string &fileName, const std::string &targetDir, int type) {
	// Match the extension case-insensitively: web caches name files after the
	// source URL, so an uppercase ".ZIP" (e.g. modland/Fujiology "SMELLS.ZIP")
	// must still open. A case-sensitive endsWith(".zip") returned nullptr here,
	// and callers that detected the archive by magic then dereferenced it.
	auto lower = utils::toLower(fileName);
	if(type == TYPE_ZIP || utils::endsWith(lower, ".zip"))
		return new ZipFile(fileName, targetDir);
	else if(type == TYPE_RAR || utils::endsWith(lower, ".rar"))
		return new RarFile(fileName, targetDir);
	return nullptr;
}

bool Archive::canHandle(const std::string &name) {
	return utils::endsWith(utils::toLower(name), ".zip");
}

} // namespace utils
