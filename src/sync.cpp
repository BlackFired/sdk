/**
 * @file sync.cpp
 * @brief Class for synchronizing local and remote trees
 *
 * (c) 2013-2014 by Mega Limited, Auckland, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#include "mega.h"

#ifdef ENABLE_SYNC
#include "mega/sync.h"
#include "mega/megaapp.h"
#include "mega/transfer.h"
#include "mega/megaclient.h"
#include "mega/base64.h"

namespace mega {
// new Syncs are automatically inserted into the session's syncs list
// and a full read of the subtree is initiated
Sync::Sync(MegaClient* cclient, string* crootpath, const char* cdebris,
           string* clocaldebris, Node* remotenode, fsfp_t cfsfp, bool cinshare, int ctag)
{
    client = cclient;
    tag = ctag;
    inshare = cinshare;
    errorcode = API_OK;
    tmpfa = NULL;

    localbytes = 0;
    localnodes[FILENODE] = 0;
    localnodes[FOLDERNODE] = 0;

    state = SYNC_INITIALSCAN;
    statecachetable = NULL;

    fullscan = true;
    scanseqno = 0;

    if (cdebris)
    {
        debris = cdebris;
        client->fsaccess->path2local(&debris, &localdebris);

        dirnotify = client->fsaccess->newdirnotify(crootpath, &localdebris);

        localdebris.insert(0, client->fsaccess->localseparator);
        localdebris.insert(0, *crootpath);
    }
    else
    {
        localdebris = *clocaldebris;

        // FIXME: pass last segment of localdebris
        dirnotify = client->fsaccess->newdirnotify(crootpath, &localdebris);
    }

    // set specified fsfp or get from fs if none
    if (cfsfp)
    {
        fsfp = cfsfp;
    }
    else
    {
        fsfp = dirnotify->fsfingerprint();
    }

    localroot.init(this, FOLDERNODE, NULL, crootpath);
    localroot.setnode(remotenode);

    sync_it = client->syncs.insert(client->syncs.end(), this);

    if (client->dbaccess)
    {
        // open state cache table
        handle tableid[3];
        string dbname;

        FileAccess *fas = client->fsaccess->newfileaccess();

        if (fas->fopen(crootpath, true, false))
        {
            tableid[0] = fas->fsid;
            tableid[1] = remotenode->nodehandle;
            tableid[2] = client->me;

            dbname.resize(sizeof tableid * 4 / 3 + 3);
            dbname.resize(Base64::btoa((byte*)tableid, sizeof tableid, (char*)dbname.c_str()));

            statecachetable = client->dbaccess->open(client->fsaccess, &dbname);

            readstatecache();
        }

        delete fas;
    }
}

Sync::~Sync()
{
    // must be set to prevent remote mass deletion while rootlocal destructor runs
    assert(state == SYNC_CANCELED || state == SYNC_FAILED);

    // unlock tmp lock
    delete tmpfa;

    // stop all active and pending downloads
    if (localroot.node)
    {
        TreeProcDelSyncGet tdsg;
        client->proctree(localroot.node, &tdsg);
    }

    delete statecachetable;

    client->syncs.erase(sync_it);
    client->syncactivity = true;
}

void Sync::addstatecachechildren(uint32_t parent_dbid, idlocalnode_map* tmap, string* path, LocalNode *p, int maxdepth)
{
    pair<idlocalnode_map::iterator,idlocalnode_map::iterator> range;
    idlocalnode_map::iterator it;
    size_t pathlen;

    range = tmap->equal_range(parent_dbid);

    pathlen = path->size();

    path->append(client->fsaccess->localseparator);

    for (it = range.first; it != range.second; it++)
    {
        path->resize(pathlen + client->fsaccess->localseparator.size());
        path->append(it->second->localname);

        LocalNode* l = it->second;
        Node* node = l->node;
        handle fsid = l->fsid;
        m_off_t size = l->size;

        // clear localname to force newnode = true in setnameparent
        l->localname.clear();

        l->init(this, l->type, p, path);

        l->parent_dbid = parent_dbid;
        l->size = size;
        l->setfsid(fsid);
        l->setnode(node);

        if (maxdepth)
        {
            addstatecachechildren(l->dbid, tmap, path, l, maxdepth - 1);
        }
    }

    path->resize(pathlen);
}

bool Sync::readstatecache()
{
    if (statecachetable && state == SYNC_INITIALSCAN)
    {
        string cachedata;
        idlocalnode_map tmap;
        uint32_t cid;
        LocalNode* l;

        statecachetable->rewind();

        // bulk-load cached nodes into tmap
        while (statecachetable->next(&cid, &cachedata, &client->key))
        {
            if ((l = LocalNode::unserialize(this, &cachedata)))
            {
                l->dbid = cid;
                tmap.insert(pair<int32_t,LocalNode*>(l->parent_dbid,l));
            }
        }

        // recursively build LocalNode tree, set scanseqnos to sync's current scanseqno
        addstatecachechildren(0, &tmap, &localroot.localname, &localroot, 100);

        // trigger a single-pass full scan to identify deleted nodes
        fullscan = true;
        scanseqno++;

        return true;
    }

    return false;
}

// remove LocalNode from DB cache
void Sync::statecachedel(LocalNode* l)
{
    if (state == SYNC_CANCELED)
    {
        return;
    }

    insertq.erase(l);

    if (l->dbid)
    {
        deleteq.insert(l->dbid);
    }
}

// insert LocalNode into DB cache
void Sync::statecacheadd(LocalNode* l)
{
    if (state == SYNC_CANCELED)
    {
        return;
    }

    if (l->dbid)
    {
        deleteq.erase(l->dbid);
    }

    insertq.insert(l);
}

void Sync::cachenodes()
{
    if (statecachetable && (state == SYNC_ACTIVE || (state == SYNC_INITIALSCAN && insertq.size() > 100)) && (deleteq.size() || insertq.size()))
    {
        LOG_debug << "Saving LocalNode database with " << insertq.size() << " additions and " << deleteq.size() << " deletions";
        statecachetable->begin();

        // deletions
        for (set<int32_t>::iterator it = deleteq.begin(); it != deleteq.end(); it++)
        {
            statecachetable->del(*it);
        }

        deleteq.clear();

        // additions - we iterate until completion or until we get stuck
        bool added;

        do {
            added = false;

            for (set<LocalNode*>::iterator it = insertq.begin(); it != insertq.end(); )
            {
                if ((*it)->parent->dbid || (*it)->parent == &localroot)
                {
                    statecachetable->put(MegaClient::CACHEDLOCALNODE, *it, &client->key);
                    insertq.erase(it++);
                    added = true;
                }
                else it++;
            }
        } while (added);

        statecachetable->commit();

        if (insertq.size())
        {
            LOG_err << "LocalNode caching did not complete";
        }
    }
}

void Sync::changestate(syncstate_t newstate)
{
    if (newstate != state)
    {
        client->app->syncupdate_state(this, newstate);

        state = newstate;
        fullscan = false;
    }
}

// walk path and return corresponding LocalNode and its parent
// path must be relative to l or start with the root prefix if l == NULL
// path must be a full sync path, i.e. start with localroot->localname
// NULL: no match, optionally returns residual path
LocalNode* Sync::localnodebypath(LocalNode* l, string* localpath, LocalNode** parent, string* rpath)
{
    const char* ptr = localpath->data();
    const char* end = ptr + localpath->size();
    size_t separatorlen = client->fsaccess->localseparator.size();

    if (rpath)
    {
        assert(!rpath->size());
    }

    if (!l)
    {
        // verify matching localroot prefix - this should always succeed for
        // internal use
        if (memcmp(ptr, localroot.localname.data(), localroot.localname.size())
         || memcmp(ptr + localroot.localname.size(),
                   client->fsaccess->localseparator.data(),
                   separatorlen))
        {
            if (parent)
            {
                *parent = NULL;
            }

            return NULL;
        }

        l = &localroot;
        ptr += l->localname.size() + client->fsaccess->localseparator.size();
    }

    const char* nptr = ptr;
    localnode_map::iterator it;
    string t;

    for (;;)
    {
        if (nptr > end)
        {
            string utf8path;
            client->fsaccess->local2path(localpath, &utf8path);
            LOG_err << "Invalid parameter in localnodebypath: " << utf8path << "  Size: " << localpath->size();

            if (rpath)
            {
                rpath->clear();
            }

            return NULL;
        }

        if (nptr == end || !memcmp(nptr, client->fsaccess->localseparator.data(), separatorlen))
        {
            if (parent)
            {
                *parent = l;
            }

            t.assign(ptr, nptr - ptr);
            if ((it = l->children.find(&t)) == l->children.end()
             && (it = l->schildren.find(&t)) == l->schildren.end())
            {
                // no full match: store residual path, return NULL with the
                // matching component LocalNode in parent
                if (rpath)
                {
                    rpath->assign(ptr, localpath->data() - ptr + localpath->size());
                }

                return NULL;
            }

            l = it->second;

            if (nptr == end)
            {
                // full match: no residual path, return corresponding LocalNode
                if (rpath)
                {
                    rpath->clear();
                }

                return l;
            }

            ptr = nptr + separatorlen;
            nptr = ptr;
        }
        else
        {
            nptr += separatorlen;
        }
    }
}

// scan localpath, add or update child nodes, call recursively for folder nodes
// localpath must be prefixed with Sync
bool Sync::scan(string* localpath, FileAccess* fa)
{
    if (localpath->size() < localdebris.size()
     || memcmp(localpath->data(), localdebris.data(), localdebris.size())
     || (localpath->size() != localdebris.size()
      && memcmp(localpath->data() + localdebris.size(),
                client->fsaccess->localseparator.data(),
                client->fsaccess->localseparator.size())))
    {
        DirAccess* da;
        string localname, name;
        bool success;

        da = client->fsaccess->newdiraccess();

        // scan the dir, mark all items with a unique identifier
        if ((success = da->dopen(localpath, fa, false)))
        {
            size_t t = localpath->size();

            while (da->dnext(localpath, &localname, client->followsymlinks))
            {
                name = localname;
                client->fsaccess->local2name(&name);

                // check if this record is to be ignored
                if (client->app->sync_syncable(name.c_str(), localpath, &localname))
                {
                    if (t)
                    {
                        localpath->append(client->fsaccess->localseparator);
                    }

                    localpath->append(localname);

                    // skip the sync's debris folder
                    if (localpath->size() < localdebris.size()
                     || memcmp(localpath->data(), localdebris.data(), localdebris.size())
                     || (localpath->size() != localdebris.size()
                      && memcmp(localpath->data() + localdebris.size(),
                                client->fsaccess->localseparator.data(),
                                client->fsaccess->localseparator.size())))
                    {
                        // new or existing record: place scan result in notification queue
                        dirnotify->notify(DirNotify::DIREVENTS, NULL, localpath->data(), localpath->size(), true);
                    }

                    localpath->resize(t);
                }
            }
        }

        delete da;

        return success;
    }
    else return false;
}

// check local path - if !localname, localpath is relative to l, with l == NULL
// being the root of the sync
// if localname is set, localpath is absolute and localname its last component
// path references a new FOLDERNODE: returns created node
// path references a existing FILENODE: returns node
// otherwise, returns NULL
LocalNode* Sync::checkpath(LocalNode* l, string* localpath, string* localname)
{
    LocalNode* ll = l;
    FileAccess* fa;
    bool newnode = false, changed = false;
    bool isroot;

    LocalNode* parent;
    string path;        // UTF-8 representation of tmppath
    string tmppath;     // full path represented by l + localpath
    string newname;     // portion of tmppath not covered by the existing
                        // LocalNode structure (always the last path component
                        // that does not have a corresponding LocalNode yet)

    if (localname)
    {
        // shortcut case (from within syncdown())
        isroot = false;
        parent = l;
        l = NULL;

        client->fsaccess->local2path(localpath, &path);
    }
    else
    {
        // construct full filesystem path in tmppath
        if (l)
        {
            l->getlocalpath(&tmppath);
        }

        if (localpath->size())
        {
            if (tmppath.size())
            {
                tmppath.append(client->fsaccess->localseparator);
            }

            tmppath.append(*localpath);
        }

        // look up deepest existing LocalNode by path, store remainder (if any)
        // in newname
        l = localnodebypath(l, localpath, &parent, &newname);

        size_t index = 0;
        while ((index = newname.find(client->fsaccess->localseparator, index)) != string::npos)
        {
            if(!(index % client->fsaccess->localseparator.size()))
            {
                LOG_warn << "Parent not detected yet. Unknown reminder: " << newname;
                string parentpath = localpath->substr(0, localpath->size() - newname.size() + index);
                dirnotify->notify(DirNotify::DIREVENTS, NULL, parentpath.data(), parentpath.size(), true);
                return NULL;
            }

            LOG_debug << "Skipping invalid separator detection";
            index++;
        }

        client->fsaccess->local2path(&tmppath, &path);

        // path invalid?
        if (!l && !newname.size())
        {
            LOG_warn << "Invalid path: " << path;
            return NULL;
        }

        string name = newname;
        client->fsaccess->local2name(&name);

        if (!client->app->sync_syncable(name.c_str(), &tmppath, &newname))
        {
            LOG_debug << "Excluded path: " << path;
            return NULL;
        }

        isroot = l == &localroot && !newname.size();
    }

    LOG_verbose << "Scanning: " << path;

    // postpone moving nodes into nonexistent parents
    if (parent && !parent->node)
    {
        LOG_warn << "Parent doesn't exist yet: " << path;
        return (LocalNode*)~0;
    }

    // attempt to open/type this file
    fa = client->fsaccess->newfileaccess();

    if (fa->fopen(localname ? localpath : &tmppath, true, false))
    {
        // match cached LocalNode state during initial/rescan to prevent costly re-fingerprinting
        // (just compare the fsids, sizes and mtimes to detect changes)
        if (fullscan)
        {
            // find corresponding LocalNode by file-/foldername
            int lastpart = client->fsaccess->lastpartlocal(localname ? localpath : &tmppath);

            string fname(localname ? *localpath : tmppath,
                         lastpart,
                         (localname ? *localpath : tmppath).size() - lastpart);

            LocalNode* cl = (parent ? parent : &localroot)->childbyname(&fname);

            if (cl && fa->fsidvalid && fa->fsid == cl->fsid)
            {
                // node found and same file
                l = cl;
                l->deleted = false;
                l->setnotseen(0);

                // if it's a file, size and mtime must match to qualify
                if (l->type != FILENODE || (l->size == fa->size && l->mtime == fa->mtime))
                {
                    l->scanseqno = scanseqno;

                    if (l->type == FOLDERNODE)
                    {
                        scan(localname ? localpath : &tmppath, fa);
                    }
                    else
                    {
                        localbytes += l->size;
                    }

                    delete fa;
                    return l;
                }
            }
        }

        if (!isroot)
        {
            if (l)
            {
                if (l->type == fa->type)
                {
                    // mark as present
                    l->setnotseen(0);

                    if (fa->type == FILENODE)
                    {
                        // has the file been overwritten or changed since the last scan?
                        // or did the size or mtime change?
                        if (fa->fsidvalid)
                        {
                            // if fsid has changed, the file was overwritten
                            // (FIXME: handle type changes)
                            if (l->fsid != fa->fsid)
                            {
                                handlelocalnode_map::iterator it;

                                // was the file overwritten by moving an existing file over it?
                                if ((it = client->fsidnode.find(fa->fsid)) != client->fsidnode.end())
                                {
                                    // catch the not so unlikely case of a false fsid match due to
                                    // e.g. a file deletion/creation cycle that reuses the same inode
                                    if (it->second->mtime != fa->mtime || it->second->size != fa->size)
                                    {
                                        // do not delete the LocalNode if it is in a different filesystem
                                        // because it could be a file with the same fsid in another drive
                                        fsfp_t fp1, fp2;
                                        if (l->sync == it->second->sync
                                            || ((fp1 = l->sync->dirnotify->fsfingerprint())
                                                && (fp2 = it->second->sync->dirnotify->fsfingerprint())
                                                && (fp1 == fp2)))
                                        {
                                            delete it->second;
                                        }
                                    }
                                    else
                                    {
                                        LOG_debug << "File move/overwrite detected";

                                        // delete existing LocalNode...
                                        delete l;

                                        // ...move remote node out of the way...
                                        client->execsyncdeletions();

                                        // ...and atomically replace with moved one
                                        client->app->syncupdate_local_move(this, it->second, path.c_str());

                                        // (in case of a move, this synchronously updates l->parent and l->node->parent)
                                        it->second->setnameparent(parent, localname ? localpath : &tmppath);

                                        // mark as seen / undo possible deletion
                                        it->second->setnotseen(0);

                                        statecacheadd(it->second);

                                        delete fa;
                                        return it->second;
                                    }
                                }
                                else
                                {
                                    l->mtime = -1;  // trigger change detection
                                }
                            }
                        }

                        // no fsid change detected or overwrite with unknown file:
                        if (fa->mtime != l->mtime || fa->size != l->size)
                        {
                            if (fa->fsidvalid && l->fsid != fa->fsid)
                            {
                                l->setfsid(fa->fsid);
                            }

                            m_off_t dsize = l->size > 0 ? l->size : 0;

                            if (l->genfingerprint(fa) && l->size >= 0)
                            {
                                localbytes -= dsize - l->size;
                            }

                            client->app->syncupdate_local_file_change(this, l, path.c_str());

                            client->stopxfer(l);
                            l->bumpnagleds();
                            l->deleted = false;

                            client->syncactivity = true;

                            statecacheadd(l);

                            delete fa;
                            return l;
                        }
                    }
                    else
                    {
                        // (we tolerate overwritten folders, because we do a
                        // content scan anyway)
                        if (fa->fsidvalid)
                        {
                            l->setfsid(fa->fsid);
                        }
                    }
                }
                else
                {
                    LOG_debug << "node type changed: recreate";
                    delete l;
                    l = NULL;
                }
            }

            // new node
            if (!l)
            {
                // rename or move of existing node?
                handlelocalnode_map::iterator it;
                fsfp_t fp1, fp2;
                if (fa->fsidvalid && (it = client->fsidnode.find(fa->fsid)) != client->fsidnode.end()
                    // additional checks to prevent wrong fsid matches
                    && it->second->type == fa->type
                    && (!parent
                        || (it->second->sync == parent->sync)
                        || ((fp1 = it->second->sync->dirnotify->fsfingerprint())
                            && (fp2 = parent->sync->dirnotify->fsfingerprint())
                            && (fp1 == fp2)))
                    && ((it->second->type != FILENODE)
                        || (it->second->mtime == fa->mtime && it->second->size == fa->size)))
                {
                    LOG_debug << "Move detected by fsid in checkpath. Type: " << it->second->type;
                    client->app->syncupdate_local_move(this, it->second, path.c_str());

                    // (in case of a move, this synchronously updates l->parent
                    // and l->node->parent)
                    it->second->setnameparent(parent, localname ? localpath : &tmppath);

                    // make sure that active PUTs receive their updated filenames
                    client->updateputs();

                    statecacheadd(it->second);

                    // unmark possible deletion
                    it->second->setnotseen(0);

                    // immediately scan folder to detect deviations from cached state
                    if (fullscan)
                    {
                        scan(localname ? localpath : &tmppath, fa);
                    }
                }
                else
                {
                    // this is a new node: add
                    LOG_debug << "New localnode.  Parent: " << (parent ? parent->name : "NO");
                    l = new LocalNode;
                    l->init(this, fa->type, parent, localname ? localpath : &tmppath);

                    if (fa->fsidvalid)
                    {
                        l->setfsid(fa->fsid);
                    }

                    newnode = true;
                }
            }
        }

        if (l)
        {
            // detect file changes or recurse into new subfolders
            if (l->type == FOLDERNODE)
            {
                if (newnode)
                {
                    scan(localname ? localpath : &tmppath, fa);
                    client->app->syncupdate_local_folder_addition(this, l, path.c_str());

                    if (!isroot)
                    {
                        statecacheadd(l);
                    }
                }
                else
                {
                    l = NULL;
                }
            }
            else
            {
                if (isroot)
                {
                    // root node cannot be a file
                    LOG_err << "The local root node is a file";
                    errorcode = API_EFAILED;
                    changestate(SYNC_FAILED);
                }
                else
                {
                    if (l->size > 0)
                    {
                        localbytes -= l->size;
                    }

                    if (l->genfingerprint(fa))
                    {
                        changed = true;
                        l->bumpnagleds();
                        l->deleted = false;
                    }

                    if (l->size > 0)
                    {
                        localbytes += l->size;
                    }

                    if (newnode)
                    {
                        client->app->syncupdate_local_file_addition(this, l, path.c_str());
                    }
                    else if (changed)
                    {
                        client->app->syncupdate_local_file_change(this, l, path.c_str());
                    }

                    if (newnode || changed)
                    {
                        statecacheadd(l);
                    }
                }
            }
        }

        if (changed || newnode)
        {
            client->syncactivity = true;
        }
    }
    else
    {
        LOG_warn << "Error opening file";
        if (fa->retry)
        {
            // fopen() signals that the failure is potentially transient - do
            // nothing and request a recheck
            dirnotify->notify(DirNotify::RETRY, ll, localpath->data(), localpath->size());
        }
        else if (l)
        {
            // immediately stop outgoing transfer, if any
            if (l->transfer)
            {
                client->stopxfer(l);
            }

            client->syncactivity = true;

            // in fullscan mode, missing files are handled in bulk in deletemissing()
            // rather than through setnotseen()
            if (!fullscan)
            {
                l->setnotseen(1);
            }
        }

        l = NULL;
    }

    delete fa;

    return l;
}

// add or refresh local filesystem item from scan stack, add items to scan stack
// returns 0 if a parent node is missing, ~0 if control should be yielded, or the time
// until a retry should be made (300 ms minimum latency).
dstime Sync::procscanq(int q)
{
    size_t t = dirnotify->notifyq[q].size();
    dstime dsmin = Waiter::ds - 3;
    LocalNode* l;

    while (t--)
    {
        LOG_verbose << "Scanning... Remaining files: " << t;

        if (dirnotify->notifyq[q].front().timestamp > dsmin)
        {
            LOG_verbose << "Scanning postponed. Modification too recent";
            return dirnotify->notifyq[q].front().timestamp - dsmin;
        }

        if ((l = dirnotify->notifyq[q].front().localnode) != (LocalNode*)~0)
        {
            l = checkpath(l, &dirnotify->notifyq[q].front().path);

            // defer processing because of a missing parent node?
            if (l == (LocalNode*)~0)
            {
                LOG_verbose << "Scanning deferred";
                return 0;
            }
        }
        else
        {
            string utf8path;
            client->fsaccess->local2path(&dirnotify->notifyq[q].front().path, &utf8path);
            LOG_debug << "Notification skipped: " << utf8path;
        }

        dirnotify->notifyq[q].pop_front();

        // we return control to the application in case a filenode was added
        // (in order to avoid lengthy blocking episodes due to multiple
        // consecutive fingerprint calculations)
        // or if new nodes are being added due to a copy/delete operation
        if ((l && l != (LocalNode*)~0 && l->type == FILENODE) || client->syncadding)
        {
            break;
        }
    }

    if (dirnotify->notifyq[q].size())
    {
        if (q == DirNotify::DIREVENTS)
        {
            client->syncactivity = true;
        }
    }
    else if (!dirnotify->notifyq[!q].size())
    {
        cachenodes();
    }

    return ~0;
}

// delete all child LocalNodes that have been missing for two consecutive scans (*l must still exist)
void Sync::deletemissing(LocalNode* l)
{
    for (localnode_map::iterator it = l->children.begin(); it != l->children.end(); )
    {
        if (scanseqno-it->second->scanseqno > 1)
        {
            delete it++->second;
        }
        else
        {
            deletemissing(it->second);
            it++;
        }
    }
}

bool Sync::movetolocaldebris(string* localpath)
{
    size_t t = localdebris.size();
    char buf[32];
    time_t ts = time(NULL);
    struct tm* ptm = localtime(&ts);
    string day, localday;
    bool havedir = false;

    for (int i = -3; i < 100; i++)
    {
        if (i == -2 || i > 95)
        {
            client->fsaccess->mkdirlocal(&localdebris, true);
        }

        sprintf(buf, "%04d-%02d-%02d", ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday);

        if (i >= 0)
        {
            sprintf(strchr(buf, 0), " %02d.%02d.%02d.%02d", ptm->tm_hour,  ptm->tm_min, ptm->tm_sec, i);
        }

        day = buf;
        client->fsaccess->path2local(&day, &localday);

        localdebris.append(client->fsaccess->localseparator);
        localdebris.append(localday);

        if (i > -3)
        {
            havedir = client->fsaccess->mkdirlocal(&localdebris, true) || client->fsaccess->target_exists;
        }

        localdebris.append(client->fsaccess->localseparator);
        localdebris.append(*localpath, client->fsaccess->lastpartlocal(localpath), string::npos);

        if (client->fsaccess->renamelocal(localpath, &localdebris, false))
        {
            localdebris.resize(t);
            return true;
        }

        localdebris.resize(t);

        if (client->fsaccess->transient_error)
        {
            return false;
        }

        if (havedir && !client->fsaccess->target_exists)
        {
            return false;
        }
    }

    return false;
}
} // namespace
#endif
