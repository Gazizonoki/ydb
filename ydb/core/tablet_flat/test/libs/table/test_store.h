#pragma once

#include "ydb/core/tablet_flat/flat_table_part.h"
#include <ydb/core/tablet_flat/flat_page_label.h>
#include <ydb/core/tablet_flat/flat_part_iface.h>
#include <ydb/core/tablet_flat/flat_sausage_misc.h>
#include <ydb/core/tablet_flat/flat_util_binary.h>
#include <ydb/core/tablet_flat/util_deref.h>
#include <ydb/core/tablet_flat/util_fmt_abort.h>

#include <util/generic/xrange.h>
#include <array>

namespace NKikimr {
namespace NTable {
namespace NTest {

    class TStore : public TAtomicRefCount<TStore> {
        enum : ui32 {
            MainPageCollection = 0,
        };

    public:
        using TData = const TSharedData;

        struct TEggs {
            bool Rooted;
            TVector<TPageId> FlatGroupIndexes;
            TVector<TPageId> FlatHistoricIndexes;
            TVector<NPage::TBtreeIndexMeta> BTreeGroupIndexes;
            TVector<NPage::TBtreeIndexMeta> BTreeHistoricIndexes;
            TData *Scheme;
            TData *Blobs;
            TData *ByKey;
            TData *Large;
            TData *Small;
            TData *GarbageStats;
            TData *TxIdStats;
        };

        ui32 GetGroupCount() const noexcept {
            return Groups;
        }

        ui32 GetRoomCount() const noexcept {
            return PageCollections.size();
        }

        ui32 GetOuterRoom() const noexcept {
            return Groups;
        }

        ui32 GetExternRoom() const noexcept {
            return Groups + 1;
        }

        const TSharedData* GetPage(ui32 room, ui32 page) const
        {
            Y_ENSURE(room < PageCollections.size(), "Room is out of bounds");

            if (page == Max<TPageId>()) return nullptr;

            return &PageCollections.at(room).at(page);
        }

        size_t GetPageSize(ui32 room, ui32 page) const
        {
            Y_ENSURE(room < PageCollections.size(), "Room is out of bounds");

            return PageCollections.at(room).at(page).size();
        }

        NPage::EPage GetPageType(ui32 room, ui32 page) const
        {
            Y_ENSURE(room < PageCollections.size(), "Room is out of bounds");

            return PageTypes.at(room).at(page);
        }

        TArrayRef<const TSharedData> PageCollectionArray(ui32 room) const
        {
            Y_ENSURE(room < PageCollections.size(), "Only regular rooms can be used as arr");

            return PageCollections[room];
        }

        NPageCollection::TGlobId GlobForBlob(ui64 ref) const
        {
            const auto& blob = PageCollections[GetExternRoom()].at(ref);

            return { TLogoBlobID(1, 2, 3, 7, blob.size(), GlobOffset + ref), /* fake group */ 123 };
        }

        ui32 PageCollectionPagesCount(ui32 room) const
        {
            return PageCollections.at(room).size();
        }

        ui64 PageCollectionBytes(ui32 room) const
        {
            auto &pages = PageCollections.at(room);

            return
                std::accumulate(pages.begin(), pages.end(), ui64(0),
                    [](ui64 bytes, const TSharedData &page) {
                        return bytes + page.size();
                    });
        }

        ui64 GetDataBytes(ui32 room) const noexcept
        {
            return DataBytes[room];
        }

        TData* GetMeta() const noexcept
        {
            return Meta ? &Meta : nullptr;
        }

        /**
         * Used for legacy part from a binary file
         */
        TEggs LegacyEggs() const
        {
            if (PageCollectionPagesCount(MainPageCollection) == 0) {
                Y_TABLET_ERROR("Cannot construct an empty part");
            }

            Y_ENSURE(!Rooted, "Legacy store must not be rooted");
            Y_ENSURE(Groups == 1, "Legacy store must have a single main group");
            Y_ENSURE(Indexes.size() == 1, "Legacy store must have a single index");
            Y_ENSURE(Scheme != Max<TPageId>(), "Legacy store is missing a scheme page");

            return {
                Rooted,
                { Indexes.back() }, 
                { },
                { },
                { },
                GetPage(MainPageCollection, Scheme),
                GetPage(MainPageCollection, Globs),
                GetPage(MainPageCollection, ByKey),
                GetPage(MainPageCollection, Large),
                nullptr,
                nullptr,
                nullptr,
            };
        }

        void Dump(IOutputStream &stream) const
        {
            NUtil::NBin::TOut out(stream);

            if (Groups > 1) {
                Y_TABLET_ERROR("Cannot dump TStore with multiple column groups");
            } else if (!PageCollections[MainPageCollection]) {
                Y_TABLET_ERROR("Cannot dump TStore with empty leader page collection");
            } else if (PageCollections[GetOuterRoom()] || PageCollections[GetExternRoom()]) {
                Y_TABLET_ERROR("TStore has auxillary rooms, cannot be dumped");
            }

            /* Dump pages as is, without any special markup as it already
                has generic labels with sufficient data to restore page collection */

            const auto& pages = PageCollections.at(MainPageCollection);

            for (auto it: xrange(pages.size())) {
                auto got = NPage::TLabelWrapper().Read(pages[it], EPage::Undef);

                Y_ENSURE(got.Page.end() == pages[it].end());

                out.Put(pages[it]);
            }
        }

        static TIntrusivePtr<TStore> Restore(IInputStream &in)
        {
            TIntrusivePtr<TStore> storage(new TStore(1));
            NPage::TLabel label;

            while (auto got = in.Load(&label, sizeof(label))) {
                Y_ENSURE(got == sizeof(label), "Invalid pages stream");

                TSharedData to = TSharedData::Uninitialized(label.Size);

                WriteUnaligned<NPage::TLabel>(to.mutable_begin(), label);

                auto *begin = to.mutable_begin() + sizeof(NPage::TLabel);

                got = in.Load(begin,  to.mutable_end() - begin);

                if (got + sizeof(NPage::TLabel) != label.Size) {
                    Y_TABLET_ERROR("Stausage loading stalled in middle of page");
                } else if (label.Type == EPage::Scheme) {
                    /* Required for Read(Evolution < 16), hack for old style
                        scheme pages without leading label. It was ecoded in
                        sample blobs with artificial label.
                     */
                    to = TSharedData::Copy(to.Slice(sizeof(NPage::TLabel)));
                }

                storage->Write(std::move(to), label.Type, 0);
            }

            storage->Finish();

            return storage;
        }

        TPageId WriteOuter(TSharedData page)
        {
            Y_ENSURE(!Finished, "This store is already finished");

            auto room = GetOuterRoom();
            TPageId pageId = PageCollections[room].size();

            PageCollections[room].emplace_back(std::move(page));
            PageTypes[room].push_back(EPage::Opaque);

            return pageId;
        }

        TPageId Write(TSharedData page, EPage type, ui32 group)
        {
            Y_ENSURE(group < PageCollections.size() - 1, "Invalid column group");
            Y_ENSURE(!Finished, "This store is already finished");
            NPageCollection::Checksum(page); /* will catch uninitialized values */

            if (type == EPage::DataPage) {
                DataBytes[group] += page.size();
            }
            TPageId pageId = PageCollections[group].size();
            PageCollections[group].emplace_back(std::move(page));
            PageTypes[group].push_back(type);

            if (group == 0) {
                switch (type) {
                    case EPage::FlatIndex:
                        Indexes.push_back(pageId);
                        break;
                    case EPage::Frames:
                        Large = pageId;
                        break;
                    case EPage::Globs:
                        Globs = pageId;
                        break;
                    case EPage::Scheme:
                    case EPage::Schem2:
                        Scheme = pageId;
                        Rooted = (type == EPage::Schem2);
                        break;
                    case EPage::Bloom:
                        ByKey = pageId;
                        break;
                    default:
                        break;
                }
            }

            return pageId;
        }

        void WriteInplace(TPageId page, TArrayRef<const char> body)
        {
            Y_ENSURE(page == Scheme);

            Meta = TSharedData::Copy(body.data(), body.size());
        }

        NPageCollection::TGlobId WriteLarge(TSharedData data)
        {
            Y_ENSURE(!Finished, "This store is already finished");

            auto room = GetExternRoom();
            TPageId pageId = PageCollections[room].size();

            PageCollections[room].emplace_back(std::move(data));
            PageTypes[room].push_back(EPage::Opaque);

            return GlobForBlob(pageId);
        }

        void Finish()
        {
            Y_ENSURE(!Finished, "Cannot finish test store more than once");
            Finished = true;
        }

        explicit TStore(size_t groups, ui32 globOffset = 0)
            : Groups(groups)
            , GlobOffset(globOffset)
            , PageCollections(groups + 2)
            , PageTypes(groups + 2)
            , DataBytes(groups + 2)
        { }

        ui32 NextGlobOffset() const {
            auto& pages = PageCollections[GetExternRoom()];
            return GlobOffset + pages.size();
        }

    private:
        const size_t Groups;
        const ui32 GlobOffset;
        TVector<TVector<TSharedData>> PageCollections;
        TVector<TVector<EPage>> PageTypes;
        TVector<ui64> DataBytes;

        /*_ Sometimes will be replaced just with one root TPageId */

        TVector<TPageId> Indexes;
        TPageId Scheme = Max<TPageId>();
        TPageId Large = Max<TPageId>();
        TPageId Globs = Max<TPageId>();
        TPageId ByKey = Max<TPageId>();
        TSharedData Meta;
        bool Rooted = false;
        bool Finished = false;
    };

}
}
}
