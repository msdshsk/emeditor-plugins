// 外部プロセスを起動し、stdin に UTF-8 を渡して stdout / stderr を UTF-8 で受け取る。
// EmEditor には依存しない。Run() は完了までブロックするので、ワーカースレッドから呼ぶこと。
//
// プラグイン（= EmEditor プロセス内の DLL）から子プロセスを起動するときの注意:
//   - ハンドル継承は PROC_THREAD_ATTRIBUTE_HANDLE_LIST でパイプだけに絞る。
//     絞らないと EmEditor や他プラグインの継承可能ハンドルまで子に渡ってしまう。
//   - 子プロセスはジョブオブジェクトに入れる。キャンセル時に子孫ごと止められ、
//     EmEditor が落ちても JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE で道連れにできる。
//   - CREATE_NO_WINDOW でコンソール窓を出さない。
#pragma once

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <string_view>
#include <thread>

namespace ee {

inline std::string WideToUtf8( std::wstring_view s )
{
	if( s.empty() )  return {};
	const int n = WideCharToMultiByte( CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr );
	std::string r( n, '\0' );
	WideCharToMultiByte( CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n, nullptr, nullptr );
	return r;
}

inline std::wstring Utf8ToWide( std::string_view s )
{
	if( s.empty() )  return {};
	const int n = MultiByteToWideChar( CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0 );
	std::wstring r( n, L'\0' );
	MultiByteToWideChar( CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n );
	return r;
}

// CommandLineToArgvW / MSVC CRT の規則で 1 引数をクォートする。
inline std::wstring QuoteArg( std::wstring_view arg )
{
	if( !arg.empty() && arg.find_first_of( L" \t\n\v\"" ) == std::wstring_view::npos ){
		return std::wstring( arg );
	}
	std::wstring r = L"\"";
	size_t backslashes = 0;
	for( wchar_t c : arg ){
		if( c == L'\\' ){
			backslashes++;
			continue;
		}
		if( c == L'"' ){
			r.append( backslashes * 2 + 1, L'\\' );   // 直前の \ を倍にし、" をエスケープ
		}
		else {
			r.append( backslashes, L'\\' );
		}
		backslashes = 0;
		r += c;
	}
	r.append( backslashes * 2, L'\\' );   // 閉じ " の直前の \ は倍にする
	r += L'"';
	return r;
}

struct ProcessResult
{
	bool        started  = false;   // CreateProcess に成功したか
	DWORD       error    = 0;       // started == false のときの GetLastError()
	bool        canceled = false;
	DWORD       exitCode = 0;
	std::string out;                // stdout（UTF-8 のまま）
	std::string err;                // stderr
};

class ProcessRunner
{
public:
	ProcessRunner()
	{
		m_hJob = CreateJobObjectW( nullptr, nullptr );
		if( m_hJob ){
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
			info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			SetInformationJobObject( m_hJob, JobObjectExtendedLimitInformation, &info, sizeof( info ) );
		}
	}

	~ProcessRunner()
	{
		if( m_hJob )  CloseHandle( m_hJob );
	}

	ProcessRunner( const ProcessRunner& ) = delete;
	ProcessRunner& operator=( const ProcessRunner& ) = delete;

	// 別スレッドから呼んでよい。実行中の子プロセスを（子孫ごと）終了させる。
	void Cancel()
	{
		m_canceled = true;
		if( m_hJob )  TerminateJobObject( m_hJob, 1 );
	}

	ProcessResult Run( const std::wstring& commandLine, const std::wstring& currentDir, const std::string& stdinData )
	{
		ProcessResult result;
		m_canceled = false;

		SECURITY_ATTRIBUTES sa = { sizeof( sa ), nullptr, TRUE };
		HANDLE hInRead = nullptr, hInWrite = nullptr;
		HANDLE hOutRead = nullptr, hOutWrite = nullptr;
		HANDLE hErrRead = nullptr, hErrWrite = nullptr;
		auto closeAll = [&]{
			for( HANDLE* p : { &hInRead, &hInWrite, &hOutRead, &hOutWrite, &hErrRead, &hErrWrite } ){
				if( *p ){ CloseHandle( *p ); *p = nullptr; }
			}
		};
		if( !CreatePipe( &hInRead, &hInWrite, &sa, 1 << 16 ) ||
			!CreatePipe( &hOutRead, &hOutWrite, &sa, 1 << 16 ) ||
			!CreatePipe( &hErrRead, &hErrWrite, &sa, 1 << 16 ) ){
			result.error = GetLastError();
			closeAll();
			return result;
		}
		// 親側の端は継承させない
		SetHandleInformation( hInWrite, HANDLE_FLAG_INHERIT, 0 );
		SetHandleInformation( hOutRead, HANDLE_FLAG_INHERIT, 0 );
		SetHandleInformation( hErrRead, HANDLE_FLAG_INHERIT, 0 );

		// 継承するハンドルを子側の 3 本に限定する
		HANDLE inherit[] = { hInRead, hOutWrite, hErrWrite };
		SIZE_T cbAttr = 0;
		InitializeProcThreadAttributeList( nullptr, 1, 0, &cbAttr );
		std::string attrBuf( cbAttr, '\0' );
		auto pAttr = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>( attrBuf.data() );
		InitializeProcThreadAttributeList( pAttr, 1, 0, &cbAttr );
		UpdateProcThreadAttribute( pAttr, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof( inherit ), nullptr, nullptr );

		STARTUPINFOEXW si = {};
		si.StartupInfo.cb = sizeof( si );
		si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		si.StartupInfo.hStdInput = hInRead;
		si.StartupInfo.hStdOutput = hOutWrite;
		si.StartupInfo.hStdError = hErrWrite;
		si.lpAttributeList = pAttr;

		PROCESS_INFORMATION pi = {};
		std::wstring cmd = commandLine;   // CreateProcessW は書き換え可能なバッファを要求する
		const BOOL ok = CreateProcessW( nullptr, cmd.data(), nullptr, nullptr, TRUE,
			CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
			nullptr, currentDir.empty() ? nullptr : currentDir.c_str(), &si.StartupInfo, &pi );
		DeleteProcThreadAttributeList( pAttr );
		if( !ok ){
			result.error = GetLastError();
			closeAll();
			return result;
		}
		result.started = true;
		if( m_hJob )  AssignProcessToJobObject( m_hJob, pi.hProcess );
		ResumeThread( pi.hThread );
		CloseHandle( pi.hThread );

		// 子側の端を閉じる（閉じないと子の終了後も ReadFile が EOF にならない）
		CloseHandle( hInRead );   hInRead = nullptr;
		CloseHandle( hOutWrite ); hOutWrite = nullptr;
		CloseHandle( hErrWrite ); hErrWrite = nullptr;

		if( m_canceled && m_hJob )  TerminateJobObject( m_hJob, 1 );   // 起動直前に Cancel された場合

		// stdin 書き込み・stderr 読み取りを別スレッドにし、パイプ満杯によるデッドロックを避ける
		std::thread writer( [&]{
			DWORD written = 0;
			size_t pos = 0;
			while( pos < stdinData.size() ){
				const DWORD chunk = (DWORD)std::min<size_t>( stdinData.size() - pos, 1 << 16 );
				if( !WriteFile( hInWrite, stdinData.data() + pos, chunk, &written, nullptr ) )  break;
				pos += written;
			}
			CloseHandle( hInWrite );
			hInWrite = nullptr;
		} );
		std::thread errReader( [&]{ ReadAll( hErrRead, result.err ); } );
		ReadAll( hOutRead, result.out );
		writer.join();
		errReader.join();

		WaitForSingleObject( pi.hProcess, INFINITE );
		GetExitCodeProcess( pi.hProcess, &result.exitCode );
		CloseHandle( pi.hProcess );
		closeAll();
		result.canceled = m_canceled;
		return result;
	}

private:
	static void ReadAll( HANDLE h, std::string& dst )
	{
		char buf[4096];
		DWORD read = 0;
		while( ReadFile( h, buf, sizeof( buf ), &read, nullptr ) && read > 0 ){
			dst.append( buf, read );
		}
	}

	HANDLE m_hJob = nullptr;
	std::atomic<bool> m_canceled = false;
};

} // namespace ee
