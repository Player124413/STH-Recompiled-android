package com.sthrecompiled.shadow;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

// The one thing the port hands to another app: the update APK in its cache, for Android's installer. It reads only
// from cacheDir/updates, never writes, and is not exported -- another app can open a file only while the installer's
// activity holds the grant the port gave it.
public final class UpdateFiles extends ContentProvider {
    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public String getType(Uri uri) {
        return "application/vnd.android.package-archive";
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sort) {
        File f = file(uri);
        MatrixCursor c = new MatrixCursor(new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE});
        if (f != null && f.isFile()) c.addRow(new Object[] {f.getName(), f.length()});
        return c;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        File f = file(uri);
        if (f == null || !f.isFile()) throw new FileNotFoundException(uri.toString());
        return ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException("read only");
    }

    @Override
    public int delete(Uri uri, String selection, String[] args) {
        throw new UnsupportedOperationException("read only");
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] args) {
        throw new UnsupportedOperationException("read only");
    }

    // one plain file name under cacheDir/updates, nothing that climbs out of it
    private File file(Uri uri) {
        String name = uri.getLastPathSegment();
        if (name == null || name.isEmpty() || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 || name.startsWith(".")) {
            return null;
        }
        if (getContext() == null) return null;
        return new File(new File(getContext().getCacheDir(), "updates"), name);
    }
}
