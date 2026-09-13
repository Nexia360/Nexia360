using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Xml;
using System.Xml.Linq;
using Microsoft.Win32.SafeHandles;

namespace Nexia.Xna;

internal sealed class PackageFileStream : FileStream {
  private readonly MemoryStream inner;

  public PackageFileStream(byte[] data)
      : base(NullHandle(), FileAccess.Read, 1) {
    inner = new MemoryStream(data, false);
  }

  private static SafeFileHandle NullHandle() {
    return File.OpenHandle(OperatingSystem.IsWindows() ? "NUL" : "/dev/null",
                           FileMode.Open, FileAccess.Read);
  }

  public override bool CanRead => true;
  public override bool CanSeek => true;
  public override bool CanWrite => false;
  public override long Length => inner.Length;

  public override long Position {
    get => inner.Position;
    set => inner.Position = value;
  }

  public override int Read(byte[] buffer, int offset, int count) =>
      inner.Read(buffer, offset, count);

  public override int Read(Span<byte> buffer) => inner.Read(buffer);

  public override int ReadByte() => inner.ReadByte();

  public override Task<int> ReadAsync(byte[] buffer, int offset, int count,
                                      CancellationToken cancellationToken) =>
      inner.ReadAsync(buffer, offset, count, cancellationToken);

  public override ValueTask<int> ReadAsync(
      Memory<byte> buffer, CancellationToken cancellationToken = default) =>
      inner.ReadAsync(buffer, cancellationToken);

  public override long Seek(long offset, SeekOrigin origin) =>
      inner.Seek(offset, origin);

  public override void CopyTo(Stream destination, int bufferSize) =>
      inner.CopyTo(destination, bufferSize);

  public override void Flush() {}

  public override void Flush(bool flushToDisk) {}

  public override Task FlushAsync(CancellationToken cancellationToken) =>
      Task.CompletedTask;

  public override void SetLength(long value) =>
      throw new NotSupportedException("The title package is read-only.");

  public override void Write(byte[] buffer, int offset, int count) =>
      throw new NotSupportedException("The title package is read-only.");

  public override void Write(ReadOnlySpan<byte> buffer) =>
      throw new NotSupportedException("The title package is read-only.");

  public override void WriteByte(byte value) =>
      throw new NotSupportedException("The title package is read-only.");

  protected override void Dispose(bool disposing) {
    if (disposing) {
      inner.Dispose();
    }
    base.Dispose(disposing);
  }
}

public static partial class CfShims {
  private static readonly DateTime PackageTimeUtc =
      new DateTime(2012, 1, 1, 0, 0, 0, DateTimeKind.Utc);

  private static bool IsTitle(string path) => NativeCalls.IsTitlePath(path);

  private static UnauthorizedAccessException ReadOnly(string path) {
    return new UnauthorizedAccessException(
        "Access to the path '" + path +
        "' is denied. The title package is read-only.");
  }

  private static byte[] TitleBytes(string path) {
    var bytes = NativeCalls.ReadTitleFile(path);
    if (bytes == null) {
      throw new FileNotFoundException(
          "Could not find file '" + path + "' in the title package.", path);
    }
    return bytes;
  }

  private static FileStream TitleStream(string path) {
    return new PackageFileStream(TitleBytes(path));
  }

  private static FileStream OpenInPackage(string path, FileMode mode,
                                          FileAccess access) {
    if (access != FileAccess.Read ||
        (mode != FileMode.Open && mode != FileMode.OpenOrCreate)) {
      throw ReadOnly(path);
    }
    return TitleStream(path);
  }

  private static StreamReader TitleReader(string path, Encoding encoding,
                                          bool detect, int bufferSize) {
    return new StreamReader(TitleStream(path), encoding ?? Encoding.UTF8,
                            detect, bufferSize > 0 ? bufferSize : -1);
  }

  private static string[] ReadLines(StreamReader reader) {
    var lines = new List<string>();
    string line;
    while ((line = reader.ReadLine()) != null) {
      lines.Add(line);
    }
    return lines.ToArray();
  }

  private static string[] TitleNames(string path, bool directories) {
    var names = NativeCalls.ListTitleDirectory(path, directories);
    if (names == null) {
      throw new DirectoryNotFoundException(
          "Could not find a part of the path '" + path +
          "' in the title package.");
    }
    return names;
  }

  private static void CollectTitleEntries(string path, string pattern,
                                          SearchOption option, bool files,
                                          bool directories,
                                          List<string> result) {
    if (files) {
      result.AddRange(MatchInPackage(path, TitleNames(path, false), pattern));
    }
    var subdirectories = TitleNames(path, true);
    if (directories) {
      result.AddRange(MatchInPackage(path, subdirectories, pattern));
    }
    if (option == SearchOption.AllDirectories) {
      foreach (var name in subdirectories) {
        CollectTitleEntries(Path.Combine(path, name), pattern, option, files,
                            directories, result);
      }
    }
  }

  private static string[] TitleEntries(string path, string pattern,
                                       SearchOption option, bool files,
                                       bool directories) {
    var result = new List<string>();
    CollectTitleEntries(path, pattern, option, files, directories, result);
    return result.ToArray();
  }

  private static FileAttributes TitleAttributes(string path) {
    if (!NativeCalls.StatTitlePath(path, out _, out bool directory)) {
      throw new FileNotFoundException(
          "Could not find file '" + path + "' in the title package.", path);
    }
    return directory ? FileAttributes.Directory | FileAttributes.ReadOnly
                     : FileAttributes.ReadOnly | FileAttributes.Archive;
  }

  private static DateTime TitleTime(string path, bool utc) {
    if (!NativeCalls.StatTitlePath(path, out _, out _)) {
      var missing = DateTime.FromFileTimeUtc(0);
      return utc ? missing : missing.ToLocalTime();
    }
    return utc ? PackageTimeUtc : PackageTimeUtc.ToLocalTime();
  }

  public static bool FileExists(string path) {
    if (!IsTitle(path)) {
      return File.Exists(path);
    }
    return NativeCalls.TitleFileExists(path);
  }

  public static FileStream FileOpen(string path, FileMode mode) {
    return IsTitle(path) ? OpenInPackage(path, mode, FileAccess.Read)
                         : File.Open(path, mode);
  }

  public static FileStream FileOpen(string path, FileMode mode,
                                    FileAccess access) {
    return IsTitle(path) ? OpenInPackage(path, mode, access)
                         : File.Open(path, mode, access);
  }

  public static FileStream FileOpen(string path, FileMode mode,
                                    FileAccess access, FileShare share) {
    return IsTitle(path) ? OpenInPackage(path, mode, access)
                         : File.Open(path, mode, access, share);
  }

  public static FileStream FileOpenRead(string path) {
    return IsTitle(path) ? TitleStream(path) : File.OpenRead(path);
  }

  public static StreamReader FileOpenText(string path) {
    return IsTitle(path) ? TitleReader(path, null, true, -1)
                         : File.OpenText(path);
  }

  public static FileStream FileOpenWrite(string path) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    return File.OpenWrite(path);
  }

  public static FileStream FileCreate(string path) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    return File.Create(path);
  }

  public static FileStream FileCreate(string path, int bufferSize) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    return File.Create(path, bufferSize);
  }

  public static FileStream FileCreate(string path, int bufferSize,
                                      FileOptions options) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    return File.Create(path, bufferSize, options);
  }

  public static StreamWriter FileCreateText(string path) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    return File.CreateText(path);
  }

  public static StreamWriter FileAppendText(string path) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    return File.AppendText(path);
  }

  public static void FileAppendAllText(string path, string contents) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    File.AppendAllText(path, contents);
  }

  public static void FileAppendAllText(string path, string contents,
                                       Encoding encoding) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    File.AppendAllText(path, contents, encoding);
  }

  public static byte[] FileReadAllBytes(string path) {
    return IsTitle(path) ? TitleBytes(path) : File.ReadAllBytes(path);
  }

  public static string FileReadAllText(string path) {
    if (!IsTitle(path)) {
      return File.ReadAllText(path);
    }
    using var reader = TitleReader(path, null, true, -1);
    return reader.ReadToEnd();
  }

  public static string FileReadAllText(string path, Encoding encoding) {
    if (!IsTitle(path)) {
      return File.ReadAllText(path, encoding);
    }
    using var reader = TitleReader(path, encoding, true, -1);
    return reader.ReadToEnd();
  }

  public static string[] FileReadAllLines(string path) {
    if (!IsTitle(path)) {
      return File.ReadAllLines(path);
    }
    using var reader = TitleReader(path, null, true, -1);
    return ReadLines(reader);
  }

  public static string[] FileReadAllLines(string path, Encoding encoding) {
    if (!IsTitle(path)) {
      return File.ReadAllLines(path, encoding);
    }
    using var reader = TitleReader(path, encoding, true, -1);
    return ReadLines(reader);
  }

  public static void FileWriteAllBytes(string path, byte[] bytes) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    File.WriteAllBytes(path, bytes);
  }

  public static void FileWriteAllText(string path, string contents) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    File.WriteAllText(path, contents);
  }

  public static void FileWriteAllText(string path, string contents,
                                      Encoding encoding) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    File.WriteAllText(path, contents, encoding);
  }

  public static void FileWriteAllLines(string path, string[] contents) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    File.WriteAllLines(path, contents);
  }

  public static void FileWriteAllLines(string path, string[] contents,
                                       Encoding encoding) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    File.WriteAllLines(path, contents, encoding);
  }

  public static void FileDelete(string path) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    File.Delete(path);
  }

  public static void FileCopy(string source, string destination) {
    FileCopy(source, destination, false);
  }

  public static void FileCopy(string source, string destination,
                              bool overwrite) {
    if (IsTitle(destination)) {
      throw ReadOnly(destination);
    }
    if (!IsTitle(source)) {
      File.Copy(source, destination, overwrite);
      return;
    }
    var bytes = TitleBytes(source);
    if (!overwrite && File.Exists(destination)) {
      throw new IOException("The file '" + destination + "' already exists.");
    }
    File.WriteAllBytes(destination, bytes);
  }

  public static void FileMove(string source, string destination) {
    if (IsTitle(source)) {
      throw ReadOnly(source);
    }
    if (IsTitle(destination)) {
      throw ReadOnly(destination);
    }
    File.Move(source, destination);
  }

  public static FileAttributes FileGetAttributes(string path) {
    return IsTitle(path) ? TitleAttributes(path) : File.GetAttributes(path);
  }

  public static void FileSetAttributes(string path, FileAttributes attributes) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    File.SetAttributes(path, attributes);
  }

  public static DateTime FileGetCreationTime(string path) {
    return IsTitle(path) ? TitleTime(path, false) : File.GetCreationTime(path);
  }

  public static DateTime FileGetCreationTimeUtc(string path) {
    return IsTitle(path) ? TitleTime(path, true)
                         : File.GetCreationTimeUtc(path);
  }

  public static DateTime FileGetLastWriteTime(string path) {
    return IsTitle(path) ? TitleTime(path, false)
                         : File.GetLastWriteTime(path);
  }

  public static DateTime FileGetLastWriteTimeUtc(string path) {
    return IsTitle(path) ? TitleTime(path, true)
                         : File.GetLastWriteTimeUtc(path);
  }

  public static DateTime FileGetLastAccessTime(string path) {
    return IsTitle(path) ? TitleTime(path, false)
                         : File.GetLastAccessTime(path);
  }

  public static DateTime FileGetLastAccessTimeUtc(string path) {
    return IsTitle(path) ? TitleTime(path, true)
                         : File.GetLastAccessTimeUtc(path);
  }

  public static void FileSetCreationTime(string path, DateTime time) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    File.SetCreationTime(path, time);
  }

  public static void FileSetCreationTimeUtc(string path, DateTime time) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    File.SetCreationTimeUtc(path, time);
  }

  public static void FileSetLastWriteTime(string path, DateTime time) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    File.SetLastWriteTime(path, time);
  }

  public static void FileSetLastWriteTimeUtc(string path, DateTime time) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    File.SetLastWriteTimeUtc(path, time);
  }

  public static void FileSetLastAccessTime(string path, DateTime time) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    File.SetLastAccessTime(path, time);
  }

  public static void FileSetLastAccessTimeUtc(string path, DateTime time) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    File.SetLastAccessTimeUtc(path, time);
  }

  public static bool DirectoryExists(string path) {
    return IsTitle(path) ? NativeCalls.TitleDirectoryExists(path)
                         : Directory.Exists(path);
  }

  public static string[] DirectoryGetFiles(string path) {
    return IsTitle(path)
               ? TitleEntries(path, null, SearchOption.TopDirectoryOnly, true,
                              false)
               : Directory.GetFiles(path);
  }

  public static string[] DirectoryGetFiles(string path, string searchPattern) {
    return IsTitle(path)
               ? TitleEntries(path, searchPattern,
                              SearchOption.TopDirectoryOnly, true, false)
               : Directory.GetFiles(path, searchPattern);
  }

  public static string[] DirectoryGetFiles(string path, string searchPattern,
                                           SearchOption option) {
    return IsTitle(path)
               ? TitleEntries(path, searchPattern, option, true, false)
               : Directory.GetFiles(path, searchPattern, option);
  }

  public static string[] DirectoryGetDirectories(string path) {
    return IsTitle(path)
               ? TitleEntries(path, null, SearchOption.TopDirectoryOnly, false,
                              true)
               : Directory.GetDirectories(path);
  }

  public static string[] DirectoryGetDirectories(string path,
                                                 string searchPattern) {
    return IsTitle(path)
               ? TitleEntries(path, searchPattern,
                              SearchOption.TopDirectoryOnly, false, true)
               : Directory.GetDirectories(path, searchPattern);
  }

  public static string[] DirectoryGetDirectories(string path,
                                                 string searchPattern,
                                                 SearchOption option) {
    return IsTitle(path)
               ? TitleEntries(path, searchPattern, option, false, true)
               : Directory.GetDirectories(path, searchPattern, option);
  }

  public static string[] DirectoryGetFileSystemEntries(string path) {
    return IsTitle(path)
               ? TitleEntries(path, null, SearchOption.TopDirectoryOnly, true,
                              true)
               : Directory.GetFileSystemEntries(path);
  }

  public static string[] DirectoryGetFileSystemEntries(string path,
                                                       string searchPattern) {
    return IsTitle(path)
               ? TitleEntries(path, searchPattern,
                              SearchOption.TopDirectoryOnly, true, true)
               : Directory.GetFileSystemEntries(path, searchPattern);
  }

  public static DirectoryInfo DirectoryCreateDirectory(string path) {
    if (!IsTitle(path)) {
      return Directory.CreateDirectory(path);
    }
    if (!NativeCalls.TitleDirectoryExists(path)) {
      throw ReadOnly(path);
    }
    return new DirectoryInfo(path);
  }

  public static void DirectoryDelete(string path) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    Directory.Delete(path);
  }

  public static void DirectoryDelete(string path, bool recursive) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    Directory.Delete(path, recursive);
  }

  public static void DirectoryMove(string source, string destination) {
    if (IsTitle(source)) {
      throw ReadOnly(source);
    }
    if (IsTitle(destination)) {
      throw ReadOnly(destination);
    }
    Directory.Move(source, destination);
  }

  public static void DirectorySetCreationTime(string path, DateTime time) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    Directory.SetCreationTime(path, time);
  }

  public static void DirectorySetCreationTimeUtc(string path, DateTime time) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    Directory.SetCreationTimeUtc(path, time);
  }

  public static void DirectorySetLastWriteTime(string path, DateTime time) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    Directory.SetLastWriteTime(path, time);
  }

  public static void DirectorySetLastWriteTimeUtc(string path, DateTime time) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    Directory.SetLastWriteTimeUtc(path, time);
  }

  public static void DirectorySetLastAccessTime(string path, DateTime time) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    Directory.SetLastAccessTime(path, time);
  }

  public static void DirectorySetLastAccessTimeUtc(string path,
                                                   DateTime time) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    Directory.SetLastAccessTimeUtc(path, time);
  }

  public static FileStream NewFileStream(string path, FileMode mode) {
    return IsTitle(path) ? OpenInPackage(path, mode, FileAccess.Read)
                         : new FileStream(path, mode);
  }

  public static FileStream NewFileStream(string path, FileMode mode,
                                         FileAccess access) {
    return IsTitle(path) ? OpenInPackage(path, mode, access)
                         : new FileStream(path, mode, access);
  }

  public static FileStream NewFileStream(string path, FileMode mode,
                                         FileAccess access, FileShare share) {
    return IsTitle(path) ? OpenInPackage(path, mode, access)
                         : new FileStream(path, mode, access, share);
  }

  public static FileStream NewFileStream(string path, FileMode mode,
                                         FileAccess access, FileShare share,
                                         int bufferSize) {
    return IsTitle(path)
               ? OpenInPackage(path, mode, access)
               : new FileStream(path, mode, access, share, bufferSize);
  }

  public static FileStream NewFileStream(string path, FileMode mode,
                                         FileAccess access, FileShare share,
                                         int bufferSize, bool useAsync) {
    return IsTitle(path)
               ? OpenInPackage(path, mode, access)
               : new FileStream(path, mode, access, share, bufferSize,
                                useAsync);
  }

  public static FileStream NewFileStream(string path, FileMode mode,
                                         FileAccess access, FileShare share,
                                         int bufferSize, FileOptions options) {
    return IsTitle(path)
               ? OpenInPackage(path, mode, access)
               : new FileStream(path, mode, access, share, bufferSize,
                                options);
  }

  public static StreamReader NewStreamReader(string path) {
    return IsTitle(path) ? TitleReader(path, null, true, -1)
                         : new StreamReader(path);
  }

  public static StreamReader NewStreamReader(string path, bool detect) {
    return IsTitle(path) ? TitleReader(path, null, detect, -1)
                         : new StreamReader(path, detect);
  }

  public static StreamReader NewStreamReader(string path, Encoding encoding) {
    return IsTitle(path) ? TitleReader(path, encoding, true, -1)
                         : new StreamReader(path, encoding);
  }

  public static StreamReader NewStreamReader(string path, Encoding encoding,
                                             bool detect) {
    return IsTitle(path) ? TitleReader(path, encoding, detect, -1)
                         : new StreamReader(path, encoding, detect);
  }

  public static StreamReader NewStreamReader(string path, Encoding encoding,
                                             bool detect, int bufferSize) {
    return IsTitle(path)
               ? TitleReader(path, encoding, detect, bufferSize)
               : new StreamReader(path, encoding, detect, bufferSize);
  }

  public static StreamWriter NewStreamWriter(string path) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    return new StreamWriter(path);
  }

  public static StreamWriter NewStreamWriter(string path, bool append) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    return new StreamWriter(path, append);
  }

  public static StreamWriter NewStreamWriter(string path, bool append,
                                             Encoding encoding) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    return new StreamWriter(path, append, encoding);
  }

  public static StreamWriter NewStreamWriter(string path, bool append,
                                             Encoding encoding,
                                             int bufferSize) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    return new StreamWriter(path, append, encoding, bufferSize);
  }

  private static string InfoPath(object self) =>
      ((FileSystemInfo)self).FullName;

  public static bool InfoExists(object self) {
    var path = InfoPath(self);
    if (!IsTitle(path)) {
      return ((FileSystemInfo)self).Exists;
    }
    return NativeCalls.StatTitlePath(path, out _, out bool directory) &&
           directory == (self is DirectoryInfo);
  }

  public static long InfoLength(object self) {
    var path = InfoPath(self);
    if (!IsTitle(path)) {
      return ((FileInfo)self).Length;
    }
    if (!NativeCalls.StatTitlePath(path, out long size, out bool directory) ||
        directory) {
      throw new FileNotFoundException(
          "Could not find file '" + path + "' in the title package.", path);
    }
    return size;
  }

  public static FileAttributes InfoGetAttributes(object self) {
    var path = InfoPath(self);
    return IsTitle(path) ? TitleAttributes(path)
                         : ((FileSystemInfo)self).Attributes;
  }

  public static void InfoSetAttributes(object self, FileAttributes value) {
    var path = InfoPath(self);
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    ((FileSystemInfo)self).Attributes = value;
  }

  public static DateTime InfoGetCreationTime(object self) {
    var path = InfoPath(self);
    return IsTitle(path) ? TitleTime(path, false)
                         : ((FileSystemInfo)self).CreationTime;
  }

  public static DateTime InfoGetCreationTimeUtc(object self) {
    var path = InfoPath(self);
    return IsTitle(path) ? TitleTime(path, true)
                         : ((FileSystemInfo)self).CreationTimeUtc;
  }

  public static DateTime InfoGetLastWriteTime(object self) {
    var path = InfoPath(self);
    return IsTitle(path) ? TitleTime(path, false)
                         : ((FileSystemInfo)self).LastWriteTime;
  }

  public static DateTime InfoGetLastWriteTimeUtc(object self) {
    var path = InfoPath(self);
    return IsTitle(path) ? TitleTime(path, true)
                         : ((FileSystemInfo)self).LastWriteTimeUtc;
  }

  public static DateTime InfoGetLastAccessTime(object self) {
    var path = InfoPath(self);
    return IsTitle(path) ? TitleTime(path, false)
                         : ((FileSystemInfo)self).LastAccessTime;
  }

  public static DateTime InfoGetLastAccessTimeUtc(object self) {
    var path = InfoPath(self);
    return IsTitle(path) ? TitleTime(path, true)
                         : ((FileSystemInfo)self).LastAccessTimeUtc;
  }

  private static FileSystemInfo WritableInfo(object self) {
    var path = InfoPath(self);
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    return (FileSystemInfo)self;
  }

  public static void InfoSetCreationTime(object self, DateTime value) {
    WritableInfo(self).CreationTime = value;
  }

  public static void InfoSetCreationTimeUtc(object self, DateTime value) {
    WritableInfo(self).CreationTimeUtc = value;
  }

  public static void InfoSetLastWriteTime(object self, DateTime value) {
    WritableInfo(self).LastWriteTime = value;
  }

  public static void InfoSetLastWriteTimeUtc(object self, DateTime value) {
    WritableInfo(self).LastWriteTimeUtc = value;
  }

  public static void InfoSetLastAccessTime(object self, DateTime value) {
    WritableInfo(self).LastAccessTime = value;
  }

  public static void InfoSetLastAccessTimeUtc(object self, DateTime value) {
    WritableInfo(self).LastAccessTimeUtc = value;
  }

  public static void InfoDelete(object self) {
    var path = InfoPath(self);
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    ((FileSystemInfo)self).Delete();
  }

  public static void InfoDelete(object self, bool recursive) {
    var path = InfoPath(self);
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    ((DirectoryInfo)self).Delete(recursive);
  }

  public static FileStream InfoOpenRead(object self) {
    var path = InfoPath(self);
    return IsTitle(path) ? TitleStream(path) : ((FileInfo)self).OpenRead();
  }

  public static StreamReader InfoOpenText(object self) {
    var path = InfoPath(self);
    return IsTitle(path) ? TitleReader(path, null, true, -1)
                         : ((FileInfo)self).OpenText();
  }

  public static FileStream InfoOpen(object self, FileMode mode) {
    var path = InfoPath(self);
    return IsTitle(path) ? OpenInPackage(path, mode, FileAccess.Read)
                         : ((FileInfo)self).Open(mode);
  }

  public static FileStream InfoOpen(object self, FileMode mode,
                                    FileAccess access) {
    var path = InfoPath(self);
    return IsTitle(path) ? OpenInPackage(path, mode, access)
                         : ((FileInfo)self).Open(mode, access);
  }

  public static FileStream InfoOpen(object self, FileMode mode,
                                    FileAccess access, FileShare share) {
    var path = InfoPath(self);
    return IsTitle(path) ? OpenInPackage(path, mode, access)
                         : ((FileInfo)self).Open(mode, access, share);
  }

  public static FileStream InfoOpenWrite(object self) {
    var path = InfoPath(self);
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    return ((FileInfo)self).OpenWrite();
  }

  public static FileStream InfoCreateFile(object self) {
    var path = InfoPath(self);
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    return ((FileInfo)self).Create();
  }

  public static StreamWriter InfoCreateText(object self) {
    var path = InfoPath(self);
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    return ((FileInfo)self).CreateText();
  }

  public static StreamWriter InfoAppendText(object self) {
    var path = InfoPath(self);
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    return ((FileInfo)self).AppendText();
  }

  public static FileInfo InfoCopyTo(object self, string destination) {
    return InfoCopyTo(self, destination, false);
  }

  public static FileInfo InfoCopyTo(object self, string destination,
                                    bool overwrite) {
    FileCopy(InfoPath(self), destination, overwrite);
    return new FileInfo(destination);
  }

  public static void InfoMoveTo(object self, string destination) {
    var path = InfoPath(self);
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    if (IsTitle(destination)) {
      throw ReadOnly(destination);
    }
    if (self is DirectoryInfo directory) {
      directory.MoveTo(destination);
    } else {
      ((FileInfo)self).MoveTo(destination);
    }
  }

  public static FileInfo[] InfoGetFiles(object self) {
    return InfoGetFiles(self, "*", SearchOption.TopDirectoryOnly);
  }

  public static FileInfo[] InfoGetFiles(object self, string searchPattern) {
    return InfoGetFiles(self, searchPattern, SearchOption.TopDirectoryOnly);
  }

  public static FileInfo[] InfoGetFiles(object self, string searchPattern,
                                        SearchOption option) {
    var directory = (DirectoryInfo)self;
    if (!IsTitle(directory.FullName)) {
      return directory.GetFiles(searchPattern, option);
    }
    return Array.ConvertAll(
        TitleEntries(directory.FullName, searchPattern, option, true, false),
        path => new FileInfo(path));
  }

  public static DirectoryInfo[] InfoGetDirectories(object self) {
    return InfoGetDirectories(self, "*", SearchOption.TopDirectoryOnly);
  }

  public static DirectoryInfo[] InfoGetDirectories(object self,
                                                   string searchPattern) {
    return InfoGetDirectories(self, searchPattern,
                              SearchOption.TopDirectoryOnly);
  }

  public static DirectoryInfo[] InfoGetDirectories(object self,
                                                   string searchPattern,
                                                   SearchOption option) {
    var directory = (DirectoryInfo)self;
    if (!IsTitle(directory.FullName)) {
      return directory.GetDirectories(searchPattern, option);
    }
    return Array.ConvertAll(
        TitleEntries(directory.FullName, searchPattern, option, false, true),
        path => new DirectoryInfo(path));
  }

  public static FileSystemInfo[] InfoGetFileSystemInfos(object self) {
    return InfoGetFileSystemInfos(self, "*");
  }

  public static FileSystemInfo[] InfoGetFileSystemInfos(object self,
                                                        string searchPattern) {
    var directory = (DirectoryInfo)self;
    if (!IsTitle(directory.FullName)) {
      return directory.GetFileSystemInfos(searchPattern);
    }
    var result = new List<FileSystemInfo>();
    foreach (var path in TitleEntries(directory.FullName, searchPattern,
                                      SearchOption.TopDirectoryOnly, true,
                                      false)) {
      result.Add(new FileInfo(path));
    }
    foreach (var path in TitleEntries(directory.FullName, searchPattern,
                                      SearchOption.TopDirectoryOnly, false,
                                      true)) {
      result.Add(new DirectoryInfo(path));
    }
    return result.ToArray();
  }

  public static void InfoCreateDirectory(object self) {
    var path = InfoPath(self);
    if (!IsTitle(path)) {
      ((DirectoryInfo)self).Create();
      return;
    }
    if (!NativeCalls.TitleDirectoryExists(path)) {
      throw ReadOnly(path);
    }
  }

  public static DirectoryInfo InfoCreateSubdirectory(object self,
                                                     string path) {
    return DirectoryCreateDirectory(Path.Combine(InfoPath(self), path));
  }

  private static XmlReaderSettings PackageReaderSettings(
      XmlReaderSettings settings) {
    var copy = settings != null ? settings.Clone() : new XmlReaderSettings();
    copy.CloseInput = true;
    copy.XmlResolver = null;
    return copy;
  }

  public static XmlReader XmlReaderCreate(string path) {
    return IsTitle(path)
               ? XmlReader.Create(TitleStream(path), PackageReaderSettings(null))
               : XmlReader.Create(path);
  }

  public static XmlReader XmlReaderCreate(string path,
                                          XmlReaderSettings settings) {
    return IsTitle(path)
               ? XmlReader.Create(TitleStream(path),
                                  PackageReaderSettings(settings))
               : XmlReader.Create(path, settings);
  }

  public static XmlReader XmlReaderCreate(string path,
                                          XmlReaderSettings settings,
                                          XmlParserContext context) {
    return IsTitle(path)
               ? XmlReader.Create(TitleStream(path),
                                  PackageReaderSettings(settings), context)
               : XmlReader.Create(path, settings, context);
  }

  public static XmlWriter XmlWriterCreate(string path) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    return XmlWriter.Create(path);
  }

  public static XmlWriter XmlWriterCreate(string path,
                                          XmlWriterSettings settings) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    return XmlWriter.Create(path, settings);
  }

  public static void XmlDocumentLoad(object self, string path) {
    var document = (XmlDocument)self;
    if (!IsTitle(path)) {
      document.Load(path);
      return;
    }
    using var stream = TitleStream(path);
    document.Load(stream);
  }

  public static void XmlDocumentSave(object self, string path) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    ((XmlDocument)self).Save(path);
  }

  public static XmlTextReader NewXmlTextReader(string path) {
    return IsTitle(path) ? new XmlTextReader(TitleStream(path))
                         : new XmlTextReader(path);
  }

  public static XmlTextWriter NewXmlTextWriter(string path,
                                               Encoding encoding) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    return new XmlTextWriter(path, encoding);
  }

  public static XDocument XDocumentLoad(string path) {
    return XDocumentLoad(path, LoadOptions.None);
  }

  public static XDocument XDocumentLoad(string path, LoadOptions options) {
    if (!IsTitle(path)) {
      return XDocument.Load(path, options);
    }
    using var stream = TitleStream(path);
    return XDocument.Load(stream, options);
  }

  public static XElement XElementLoad(string path) {
    return XElementLoad(path, LoadOptions.None);
  }

  public static XElement XElementLoad(string path, LoadOptions options) {
    if (!IsTitle(path)) {
      return XElement.Load(path, options);
    }
    using var stream = TitleStream(path);
    return XElement.Load(stream, options);
  }

  public static void XDocumentSave(object self, string path) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    ((XDocument)self).Save(path);
  }

  public static void XDocumentSave(object self, string path,
                                   SaveOptions options) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    ((XDocument)self).Save(path, options);
  }

  public static void XElementSave(object self, string path) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    ((XElement)self).Save(path);
  }

  public static void XElementSave(object self, string path,
                                  SaveOptions options) {
    if (IsTitle(path)) {
      throw ReadOnly(path);
    }
    ((XElement)self).Save(path, options);
  }
}
