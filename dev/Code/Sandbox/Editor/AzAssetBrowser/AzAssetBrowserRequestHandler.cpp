
/*
* All or portions of this file Copyright (c) Amazon.com, Inc. or its affiliates or
* its licensors.
*
* For complete copyright and license terms please see the LICENSE at the root of this
* distribution (the "License"). All use of this software is governed by the License,
* or, if provided, by the license below or the license accompanying this file. Do not
* remove or modify any license notices. This file is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*
*/
#include "StdAfx.h"

#include <Editor/AzAssetBrowser/AzAssetBrowserRequestHandler.h>

#include <AzCore/Asset/AssetCommon.h>
#include <AzCore/Serialization/Utils.h>
#include <AzCore/Slice/SliceAsset.h>
#include <AzCore/Slice/SliceComponent.h>
#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Component/Entity.h>
#include <AzCore/Asset/AssetManager.h>
#include <AzCore/Asset/AssetTypeInfoBus.h>
#include <AzCore/std/string/wildcard.h>

#include <AzFramework/StringFunc/StringFunc.h>
#include <AzFramework/Asset/AssetSystemBus.h>
#include <AzFramework/Asset/GenericAssetHandler.h>
#include <AzFramework/IO/FileOperations.h>

#include <AzToolsFramework/ToolsComponents/ComponentAssetMimeDataContainer.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/SourceControl/SourceControlAPI.h>
#include <AzToolsFramework/Entity/EditorEntityContextBus.h>
#include <AzToolsFramework/ToolsComponents/EditorComponentBase.h>
#include <AzToolsFramework/ToolsComponents/GenericComponentWrapper.h>
#include <AzToolsFramework/ToolsComponents/TransformComponent.h>
#include <AzToolsFramework/Commands/EntityStateCommand.h>
#include <AzToolsFramework/AssetBrowser/AssetBrowserBus.h>
#include <AzToolsFramework/AssetBrowser/AssetBrowserEntry.h>
#include <AzToolsFramework/AssetBrowser/EBusFindAssetTypeByName.h>

#include <AzToolsFramework/Metrics/LyEditorMetricsBus.h>
#include <AzToolsFramework/AssetEditor/AssetEditorBus.h>

#include <AzQtComponents/DragAndDrop/ViewportDragAndDrop.h>

#include "Viewport.h"
#include "ViewManager.h"
#include <MathConversion.h>
#include <QMessageBox>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QDesktopServices>
#include <QIcon>
#include <QFileIconProvider>

namespace AzAssetBrowserRequestHandlerPrivate
{
    using namespace AzToolsFramework;
    using namespace AzToolsFramework::AssetBrowser;
    // return true ONLY if we can handle the drop request in the viewport.
    bool CanSpawnEntityForProduct(const ProductAssetBrowserEntry* product)
    {
        if (!product)
        {
            return false;
        }

        if (product->GetAssetType() == AZ::AzTypeInfo<AZ::SliceAsset>::Uuid())
        {
            return true; // we can always spawn slices.
        }

        bool canCreateComponent = false;
        AZ::AssetTypeInfoBus::EventResult(canCreateComponent, product->GetAssetType(), &AZ::AssetTypeInfo::CanCreateComponent, product->GetAssetId());

        if (!canCreateComponent)
        {
            return false;
        }

        AZ::Uuid componentTypeId = AZ::Uuid::CreateNull();
        AZ::AssetTypeInfoBus::EventResult(componentTypeId, product->GetAssetType(), &AZ::AssetTypeInfo::GetComponentTypeId);

        if (!componentTypeId.IsNull())
        {
            // we have a component type that handles this asset.
            return true;
        }

        // additional operations can be added here.

        return false;
    }

    void SpawnEntityAtPoint(const ProductAssetBrowserEntry* product, AzQtComponents::ViewportDragContext* viewportDragContext, EntityIdList& spawnList, AzFramework::SliceInstantiationTicket& spawnTicket)
    {
        // Calculate the drop location.
        if ((!viewportDragContext) || (!product))
        {
            return;
        }

        const AZ::Transform worldTransform = AZ::Transform::CreateTranslation(viewportDragContext->m_hitLocation);

        // Handle instantiation of slices.
        if (product->GetAssetType() == AZ::AzTypeInfo<AZ::SliceAsset>::Uuid())
        {
            // Instantiate the slice at the specified location.
            AZ::Data::Asset<AZ::SliceAsset> asset = AZ::Data::AssetManager::Instance().GetAsset<AZ::SliceAsset>(product->GetAssetId(), false);
            if (asset)
            {
                AzToolsFramework::EditorMetricsEventsBusAction editorMetricsEventsBusActionWrapper(AzToolsFramework::EditorMetricsEventsBusTraits::NavigationTrigger::DragAndDrop);
                AZStd::string idString;
                asset.GetId().ToString(idString);
                AzToolsFramework::EditorMetricsEventsBus::Broadcast(&AzToolsFramework::EditorMetricsEventsBusTraits::SliceInstantiated, AZ::Crc32(idString.c_str()));

                EditorEntityContextRequestBus::BroadcastResult(spawnTicket, &EditorEntityContextRequests::InstantiateEditorSlice, asset, worldTransform);
            }
        }
        else
        {
            ScopedUndoBatch undo("Create entities from asset");

            //  Add the component(s).
            AZ::Uuid componentTypeId = AZ::Uuid::CreateNull();
            AZ::AssetTypeInfoBus::EventResult(componentTypeId, product->GetAssetType(), &AZ::AssetTypeInfo::GetComponentTypeId);
            if (!componentTypeId.IsNull())
            {
                AZStd::string entityName;

                // If the entity is being created from an asset, name it after said asset.
                const AZ::Data::AssetId assetId = product->GetAssetId();
                AZStd::string assetPath;
                AZ::Data::AssetCatalogRequestBus::BroadcastResult(assetPath, &AZ::Data::AssetCatalogRequests::GetAssetPathById, assetId);
                if (!assetPath.empty())
                {
                    AzFramework::StringFunc::Path::GetFileName(assetPath.c_str(), entityName);
                }

                // If not sourced from an asset, generate a generic name.
                if (entityName.empty())
                {
                    entityName = AZStd::string::format("Entity%d", GetIEditor()->GetObjectManager()->GetObjectCount());
                }

                AZ::Entity* newEntity = aznew AZ::Entity(entityName.c_str());
                EditorEntityContextRequestBus::Broadcast(&EditorEntityContextRequests::AddRequiredComponents, *newEntity);

                AzToolsFramework::EditorMetricsEventsBusAction editorMetricsEventsBusActionWrapper(AzToolsFramework::EditorMetricsEventsBusTraits::NavigationTrigger::DragAndDrop);

                // Create Entity metrics event (Drag+Drop from Asset Browser to Viewport)
                EditorMetricsEventsBus::Broadcast(&EditorMetricsEventsBusTraits::EntityCreated, newEntity->GetId());

                // Create component.
                AZ::Component* newComponent = newEntity->CreateComponent(componentTypeId);
                // If it's not an "editor component" then wrap it in a GenericComponentWrapper.
                bool needsGenericWrapper = azrtti_cast<AzToolsFramework::Components::EditorComponentBase*>(newComponent) == nullptr;
                if (needsGenericWrapper)
                {
                    newEntity->RemoveComponent(newComponent);
                    newComponent = aznew AzToolsFramework::Components::GenericComponentWrapper(newComponent);
                    newEntity->AddComponent(newComponent);
                }

                if (newComponent)
                {
                    // Add Component metrics event (Drag+Drop from Asset Browser to View port to create a new entity with the component)
                    EditorMetricsEventsBus::Broadcast(&EditorMetricsEventsBusTraits::ComponentAdded, newEntity->GetId(), componentTypeId);
                }

                //  Set entity position.
                auto* transformComponent = newEntity->FindComponent<Components::TransformComponent>();
                if (transformComponent)
                {
                    transformComponent->SetWorldTM(worldTransform);
                }

                // Add the entity to the editor context, which activates it and creates the sandbox object.
                EditorEntityContextRequestBus::Broadcast(&EditorEntityContextRequests::AddEditorEntity, newEntity);

                // set asset after components have been activated in AddEditorEntity method
                if (newComponent)
                {
                    Components::EditorComponentBase* asEditorComponent =
                        azrtti_cast<Components::EditorComponentBase*>(newComponent);

                    if (asEditorComponent)
                    {
                        asEditorComponent->SetPrimaryAsset(assetId);
                    }
                }

                // Prepare undo command last so it captures the final state of the entity.
                EntityCreateCommand* command = aznew EntityCreateCommand(static_cast<AZ::u64>(newEntity->GetId()));
                command->Capture(newEntity);
                command->SetParent(undo.GetUndoBatch());
                ToolsApplicationRequests::Bus::Broadcast(&ToolsApplicationRequests::AddDirtyEntity, newEntity->GetId());
                spawnList.push_back(newEntity->GetId());
            }
        }
    }

    // Helper utility - determines if the thing being dragged is a FBX from the scene import pipeline
    // This is important to differentiate.
    // when someone drags a MTL file directly into the viewport, even from a FBX, we want to spawn it as a decal
    // but when someone drags a FBX that contains MTL files, we want only to spawn the meshes.
    // so we have to specifically differentiate here between the mimeData type that contains the source as the root
    // (dragging the fbx file itself)
    // and one which contains the actual product at its root.

    bool IsDragOfFBX(const QMimeData* mimeData)
    {
        AZStd::vector<AssetBrowserEntry*> entries;
        if (!AssetBrowserEntry::FromMimeData(mimeData, entries))
        {
            // if mimedata does not even contain entries, no point in proceeding.
            return false;
        }

        for (auto entry : entries)
        {
            if (entry->GetEntryType() != AssetBrowserEntry::AssetEntryType::Source)
            {
                continue;
            }
            // this is a source file.  Is it the filetype we're looking for?
            if (SourceAssetBrowserEntry* source = azrtti_cast<SourceAssetBrowserEntry*>(entry))
            {
                if (AzFramework::StringFunc::Equal(source->GetExtension().c_str(), ".fbx", false))
                {
                    return true;
                }
            }
        }
        return false;
    }
}

AzAssetBrowserRequestHandler::AzAssetBrowserRequestHandler()
{
    AzToolsFramework::AssetBrowser::AssetBrowserInteractionNotificationBus::Handler::BusConnect();
    AzQtComponents::DragAndDropEventsBus::Handler::BusConnect(AzQtComponents::DragAndDropContexts::EditorViewport);
}

AzAssetBrowserRequestHandler::~AzAssetBrowserRequestHandler()
{
    AzToolsFramework::AssetBrowser::AssetBrowserInteractionNotificationBus::Handler::BusDisconnect();
    AzQtComponents::DragAndDropEventsBus::Handler::BusDisconnect();
}

void AzAssetBrowserRequestHandler::AddContextMenuActions(QWidget* caller, QMenu* menu, const AZStd::vector<AzToolsFramework::AssetBrowser::AssetBrowserEntry*>& entries)
{
    using namespace AzToolsFramework::AssetBrowser;

    AssetBrowserEntry* entry = entries.empty() ? nullptr : entries.front();
    if (!entry)
    {
        return;
    }

    AZStd::string fullFileDirectory;
    AZStd::string fullFilePath;
    AZStd::string fileName;

    switch (entry->GetEntryType())
    {
    case AssetBrowserEntry::AssetEntryType::Product:
        // if its a product, we actually want to perform these operations on the source
        // which will be the parent of the product.
        entry = entry->GetParent();
        if ((!entry) || (entry->GetEntryType() != AssetBrowserEntry::AssetEntryType::Source))
        {
            AZ_Assert(false, "Asset Browser entry product has a non-source parent?");
            break;     // no valid parent.
        }
    // the fall through to the next case is intentional here.
    case AssetBrowserEntry::AssetEntryType::Source:
    {
        AZ::Uuid sourceID = azrtti_cast<SourceAssetBrowserEntry*>(entry)->GetSourceUuid();
        fullFilePath = entry->GetFullPath();
        fullFileDirectory = fullFilePath.substr(0, fullFilePath.find_last_of(AZ_CORRECT_DATABASE_SEPARATOR));
        fileName = entry->GetName();

        // Add the "Open" menu item.
        // Note that source file openers are allowed to "veto" the showing of the "Open" menu if its 100% known that they aren't openable!
        // for example, custom data formats that are made by Lumberyard that can not have a program associated in the operating system to view them.
        // If the only opener that can open that file has no m_opener, then its not openable.
        SourceFileOpenerList openers;
        AssetBrowserInteractionNotificationBus::Broadcast(&AssetBrowserInteractionNotificationBus::Events::AddSourceFileOpeners, fullFilePath.c_str(), sourceID, openers);
        bool foundNonNullOpener = false;
        for (const SourceFileOpenerDetails& openerDetails : openers)
        {
            // bind that function to the current loop element.
            if (openerDetails.m_opener) // only VALID openers with an actual callback.
            {
                foundNonNullOpener = true;
                break;
            }
        }

        if (openers.empty() || foundNonNullOpener)
        {
            // if we get here either NOBODY has an opener for this kind of asset, or there is at least one opener that is not vetoing the ability to open it.
            menu->addAction(QObject::tr("Open"), menu, [sourceID]()
            {
                bool someoneHandledIt = false;
                AssetBrowserInteractionNotificationBus::Broadcast(&AssetBrowserInteractionNotifications::OpenAssetInAssociatedEditor, sourceID, someoneHandledIt);
            });
        }

        AZStd::vector<const ProductAssetBrowserEntry*> products;
        entry->GetChildrenRecursively<ProductAssetBrowserEntry>(products);
        if (products.empty())
        {
            if (entry->GetEntryType() == AssetBrowserEntry::AssetEntryType::Source)
            {
                CFileUtil::PopulateQMenu(caller, menu, fileName.c_str(), fullFileDirectory.c_str());
            }
            return;
        }
        auto productEntry = products[0];

        AZ::SerializeContext* serializeContext = nullptr;
        AZ::ComponentApplicationBus::BroadcastResult(serializeContext, &AZ::ComponentApplicationRequests::GetSerializeContext);
        const AZ::SerializeContext::ClassData* assetClassData = serializeContext->FindClassData(productEntry->GetAssetType());
        AZ_Assert(serializeContext, "Failed to retrieve serialize context.");

        // For slices, we provide the option to toggle the dynamic flag.
        QString sliceOptions[] = { QObject::tr("Set Dynamic Slice"), QObject::tr("Unset Dynamic Slice") };
        if (productEntry->GetAssetType() == AZ::AzTypeInfo<AZ::SliceAsset>::Uuid())
        {
            AZ::Entity* sliceEntity = AZ::Utils::LoadObjectFromFile<AZ::Entity>(fullFilePath, nullptr,
                    AZ::ObjectStream::FilterDescriptor(AZ::ObjectStream::AssetFilterNoAssetLoading));
            AZ::SliceComponent* sliceAsset = sliceEntity ? sliceEntity->FindComponent<AZ::SliceComponent>() : nullptr;
            if (sliceAsset)
            {
                if (sliceAsset->IsDynamic())
                {
                    menu->addAction(sliceOptions[1], [sliceEntity, fullFilePath]()
                        {
                            /*Unset dynamic slice*/
                            AZ::SliceComponent* sliceAsset = sliceEntity->FindComponent<AZ::SliceComponent>();
                            AZ_Assert(sliceAsset, "SliceComponent no longer present on component.");
                            sliceAsset->SetIsDynamic(false);
                            ResaveSlice(sliceEntity, fullFilePath);
                        });
                }
                else
                {
                    menu->addAction(sliceOptions[0], [sliceEntity, fullFilePath]()
                        {
                            /*Set dynamic slice*/
                            AZ::SliceComponent* sliceAsset = sliceEntity->FindComponent<AZ::SliceComponent>();
                            AZ_Assert(sliceAsset, "SliceComponent no longer present on component.");
                            sliceAsset->SetIsDynamic(true);
                            ResaveSlice(sliceEntity, fullFilePath);
                        });
                }
            }
            else
            {
                delete sliceEntity;
            }
        }

        CFileUtil::PopulateQMenu(caller, menu, fileName.c_str(), fullFileDirectory.c_str());
    }
    break;
    case AssetBrowserEntry::AssetEntryType::Folder:
    {
        fullFileDirectory = entry->GetFullPath();
        // we are sending an empty filename to indicate that it is a folder and not a file
        CFileUtil::PopulateQMenu(caller, menu, fileName.c_str(), fullFileDirectory.c_str());
    }
    break;
    default:
        break;
    }
}

void AzAssetBrowserRequestHandler::ResaveSlice(AZ::Entity* sliceEntity, const AZStd::string& fullFilePath)
{
    AZStd::string tmpFileName;
    bool tmpFilesaved = false;
    // here we are saving the slice to a temp file instead of the original file and then copying the temp file to the original file.
    // This ensures that AP will not a get a file change notification on an incomplete slice file causing it to fail processing. Temp files are ignored by AP.
    if (AZ::IO::CreateTempFileName(fullFilePath.c_str(), tmpFileName))
    {
        AZ::IO::FileIOStream fileStream(tmpFileName.c_str(), AZ::IO::OpenMode::ModeWrite | AZ::IO::OpenMode::ModeBinary);
       
        if (fileStream.IsOpen())
        {
            tmpFilesaved = AZ::Utils::SaveObjectToStream<AZ::Entity>(fileStream, AZ::DataStream::ST_XML, sliceEntity);
        }

        using SCCommandBus = AzToolsFramework::SourceControlCommandBus;
        SCCommandBus::Broadcast(&SCCommandBus::Events::RequestEdit, fullFilePath.c_str(), true,
            [sliceEntity, fullFilePath, tmpFileName, tmpFilesaved](bool success, const AzToolsFramework::SourceControlFileInfo& info)
        {
            if (!info.IsReadOnly())
            {
                if (tmpFilesaved && AZ::IO::SmartMove(tmpFileName.c_str(), fullFilePath.c_str()))
                {
                    // Bump the slice asset up in the asset processor's queue.
                    AzFramework::AssetSystemRequestBus::Broadcast(&AzFramework::AssetSystem::AssetSystemRequests::GetAssetStatus, fullFilePath);
                }
            }
            else
            {
                QWidget* mainWindow = nullptr;
                AzToolsFramework::EditorRequests::Bus::BroadcastResult(mainWindow, &AzToolsFramework::EditorRequests::GetMainWindow);
                QMessageBox::warning(mainWindow, QObject::tr("Unable to Modify Slice"),
                    QObject::tr("File is not writable."), QMessageBox::Ok, QMessageBox::Ok);
            }

            delete sliceEntity;
        });
    }
    else
    {
        QWidget* mainWindow = nullptr;
        AzToolsFramework::EditorRequests::Bus::BroadcastResult(mainWindow, &AzToolsFramework::EditorRequests::GetMainWindow);
        QMessageBox::warning(mainWindow, QObject::tr("Unable to Modify Slice"),
            QObject::tr("Unable to Modify Slice (%1). Cannot create a temporary file for writing data in the same folder.").arg(fullFilePath.c_str()), 
            QMessageBox::Ok, QMessageBox::Ok);
    }
}
bool AzAssetBrowserRequestHandler::CanAcceptDragAndDropEvent(QDropEvent* event, AzQtComponents::DragAndDropContextBase& context) const
{
    using namespace AzQtComponents;
    using namespace AzToolsFramework;
    using namespace AzToolsFramework::AssetBrowser;
    using namespace AzAssetBrowserRequestHandlerPrivate;

    // if a listener with a higher priority already claimed this event, do not touch it.
    ViewportDragContext* viewportDragContext = azrtti_cast<ViewportDragContext*>(&context);
    if ((!event) || (!event->mimeData()) || (event->isAccepted()) || (!viewportDragContext))
    {
        return false;
    }

    // is it something we know how to spawn?
    bool canSpawn = false;

    AzToolsFramework::AssetBrowser::AssetBrowserEntry::ForEachEntryInMimeData<ProductAssetBrowserEntry>(event->mimeData(),
        [&](const AzToolsFramework::AssetBrowser::ProductAssetBrowserEntry* product)
        {
            if (CanSpawnEntityForProduct(product))
            {
                canSpawn = true;
            }
        });

    return canSpawn;
}

void AzAssetBrowserRequestHandler::DragEnter(QDragEnterEvent* event, AzQtComponents::DragAndDropContextBase& context)
{
    if (CanAcceptDragAndDropEvent(event, context))
    {
        event->setDropAction(Qt::CopyAction);
        event->setAccepted(true);
    }
}

void AzAssetBrowserRequestHandler::DragMove(QDragMoveEvent* event, AzQtComponents::DragAndDropContextBase& context)
{
    if (CanAcceptDragAndDropEvent(event, context))
    {
        event->setDropAction(Qt::CopyAction);
        event->setAccepted(true);
    }
}

void AzAssetBrowserRequestHandler::DragLeave(QDragLeaveEvent* /*event*/)
{
    // opportunities to show ghosted entities or previews here.
}

void AzAssetBrowserRequestHandler::Drop(QDropEvent* event, AzQtComponents::DragAndDropContextBase& context)
{
    using namespace AzToolsFramework;
    using namespace AzToolsFramework::AssetBrowser;
    using namespace AzQtComponents;
    using namespace AzAssetBrowserRequestHandlerPrivate;

    // ALWAYS CHECK - you are not the only one connected to this bus, and someone else may have already
    // handled the event or accepted the drop - it might not contain types relevant to you.
    // you still get informed about the drop event in case you did some stuff in your gui and need to clean it up.
    if (!CanAcceptDragAndDropEvent(event, context))
    {
        return;
    }

    //  we wouldn't reach this code if the following cast is null or the event was null or accepted was already true.
    ViewportDragContext* viewportDragContext = azrtti_cast<ViewportDragContext*>(&context);

    event->setDropAction(Qt::CopyAction);
    event->setAccepted(true);

    // spawn entities!
    EntityIdList spawnedEntities;
    AzFramework::SliceInstantiationTicket spawnTicket;

    // make a scoped undo that covers the ENTIRE operation.
    ScopedUndoBatch undo("Create entities from asset");

    AssetBrowserEntry::ForEachEntryInMimeData<ProductAssetBrowserEntry>(event->mimeData(),
        [&](const ProductAssetBrowserEntry* product)
        {
            if (CanSpawnEntityForProduct(product))
            {
                SpawnEntityAtPoint(product, viewportDragContext, spawnedEntities, spawnTicket);
            }
        });

    // Select the new entity (and deselect others).
    if (!spawnedEntities.empty())
    {
        ToolsApplicationRequests::Bus::Broadcast(&ToolsApplicationRequests::SetSelectedEntities, spawnedEntities);
    }
}

void AzAssetBrowserRequestHandler::AddSourceFileOpeners(const char* fullSourceFileName, const AZ::Uuid& sourceUUID, AzToolsFramework::AssetBrowser::SourceFileOpenerList& openers)
{
    using namespace AzToolsFramework;

    //Get asset group to support a variety of file extensions
    const AzToolsFramework::AssetBrowser::SourceAssetBrowserEntry* fullDetails = AzToolsFramework::AssetBrowser::SourceAssetBrowserEntry::GetSourceByAssetId(sourceUUID);
    QString assetGroup;
    AZ::AssetTypeInfoBus::EventResult(assetGroup, fullDetails->GetPrimaryAssetType(), &AZ::AssetTypeInfo::GetGroup);

    if (AZStd::wildcard_match("*.lua", fullSourceFileName))
    {
        AZStd::string fullName(fullSourceFileName);
        // LUA files can be opened with the lumberyard LUA editor.
        openers.push_back(
            {
                "Lumberyard_LUA_Editor",
                "Open in Lumberyard LUA Editor...",
                QIcon(":/PropertyEditor/Resources/edit-asset.png"),
                [](const char* fullSourceFileNameInCallback, const AZ::Uuid& /*sourceUUID*/)
                {
                    // we know how to handle LUA files (open with the lua Editor.
                    EditorRequestBus::Broadcast(&EditorRequests::LaunchLuaEditor, fullSourceFileNameInCallback);
                }
            });
    }
    else if (AZStd::wildcard_match("*.slice", fullSourceFileName))
    {
        // we don't allow you to "open" regular slices, so add a nullptr to indicate
        // that we have taken care of this, don't show menus or anything.  This will be ignored if there are other openers.
        openers.push_back({"Lumberyard_Slice", "", QIcon(), nullptr });
    }
#if defined(AZ_PLATFORM_WINDOWS)
    else if (assetGroup.compare("Texture", Qt::CaseInsensitive) == 0)
    {
        //Open the texture in RC editor
        openers.push_back(
        {
            "Lumberyard_Resource_Compiler",
            "Open in Resource Compiler...",
            QIcon(":/PropertyEditor/Resources/edit-asset.png"),
            [](const char* fullSourceFileNameInCallback, const AZ::Uuid& /*sourceUUID*/)
            {
                gEnv->pResourceCompilerHelper->CallResourceCompiler(fullSourceFileNameInCallback, "/userdialog", NULL, false, IResourceCompilerHelper::eRcExePath_currentFolder, true, false, L".");
            }
        });
    }
#endif //  AZ_PLATFORM_WINDOWS


}

void AzAssetBrowserRequestHandler::OpenAssetInAssociatedEditor(const AZ::Data::AssetId& assetId, bool& alreadyHandled)
{
    using namespace AzToolsFramework::AssetBrowser;
    if (alreadyHandled)
    {
        // a higher priority listener has already taken this request.
        return;
    }
    const SourceAssetBrowserEntry* source = SourceAssetBrowserEntry::GetSourceByAssetId(assetId.m_guid);
    if (!source)
    {
        return;
    }

    AZStd::string fullEntryPath = source->GetFullPath();
    AZ::Uuid sourceID = source->GetSourceUuid();

    if (fullEntryPath.empty())
    {
        return;
    }

    QWidget* mainWindow = nullptr;
    AzToolsFramework::EditorRequestBus::BroadcastResult(mainWindow, &AzToolsFramework::EditorRequests::GetMainWindow);

    SourceFileOpenerList openers;
    AssetBrowserInteractionNotificationBus::Broadcast(&AssetBrowserInteractionNotificationBus::Events::AddSourceFileOpeners, fullEntryPath.c_str(), sourceID, openers);

    // did anyone actually accept it?
    if (!openers.empty())
    {
        // yes, call the opener and return.

        // are there more than one opener(s)?
        const SourceFileOpenerDetails* openerToUse = nullptr;
        // a function which reassigns openerToUse to be the selected one.
        AZStd::function<void(const SourceFileOpenerDetails*)> switchToOpener = [&openerToUse](const SourceFileOpenerDetails* switchTo)
            {
                openerToUse = switchTo;
            };

        // callers are allowed to add nullptr to openers.  So we only evaluate the valid ones.
        // and if there is only one valid one, we use that one.
        const SourceFileOpenerDetails* firstValidOpener = nullptr;
        int numValidOpeners = 0;
        QMenu menu(mainWindow);
        for (const SourceFileOpenerDetails& openerDetails : openers)
        {
            // bind that function to the current loop element.
            if (openerDetails.m_opener) // only VALID openers with an actual callback.
            {
                ++numValidOpeners;
                if (!firstValidOpener)
                {
                    firstValidOpener = &openerDetails;
                }
                // bind a callback such that when the menu item is clicked, it sets that as the opener to use.
                menu.addAction(openerDetails.m_iconToUse, QObject::tr(openerDetails.m_displayText.c_str()), mainWindow, AZStd::bind(switchToOpener, &openerDetails));
            }
        }

        if (numValidOpeners > 1) // more than one option was added
        {
            menu.addSeparator();
            menu.addAction(QObject::tr("Cancel"), AZStd::bind(switchToOpener, nullptr)); // just something to click on to avoid doing anything.
            menu.exec(QCursor::pos());
        }
        else if (numValidOpeners == 1)
        {
            openerToUse = firstValidOpener;
        }

        // did we select one and did it have a function to call?
        if ((openerToUse) && (openerToUse->m_opener))
        {
            openerToUse->m_opener(fullEntryPath.c_str(), sourceID);
        }
        alreadyHandled = true;
        return; // an opener handled this, no need to proceed further.
    }

    // nobody accepted it, we will try default logic here.
    // default logic is to see if its serializable via edit context, and open the asset editor if so.
    // if not, we try the operating systems file associations.
    // lets find out whether the Generic Asset handler handles this kind of asset.
    // to do so we need the actual type of that asset.
    AZ::Data::AssetManager& manager = AZ::Data::AssetManager::Instance();

    // find a product type to query against.
    AZStd::vector<const ProductAssetBrowserEntry*> candidates;

    // is the given assetId actually valid?
    ProductAssetBrowserEntry* productFound = ProductAssetBrowserEntry::GetProductByAssetId(assetId);
    if (productFound)
    {
        // if we started with a product selected, we already know which one we care about.
        candidates.push_back(productFound);
    }
    else
    {
        // the product is not valid, so just use all of the children.
        source->GetChildrenRecursively<ProductAssetBrowserEntry>(candidates);
    }

    bool foundHandler = false;
    // find the first one that is handled by something:
    for (const ProductAssetBrowserEntry* productEntry : candidates)
    {
        // is there a Generic Asset Handler for it?
        AZ::Data::AssetType productAssetType = productEntry->GetAssetType();
        if ((productAssetType == AZ::Data::s_invalidAssetType) || (!productEntry->GetAssetId().IsValid()))
        {
            continue;
        }

        if (const AZ::Data::AssetHandler* assetHandler = manager.GetHandler(productAssetType))
        {
            if (!azrtti_istypeof<AzFramework::GenericAssetHandlerBase*>(assetHandler))
            {
                // its not the generic asset handler.
                continue;
            }
            // yes, and its the generic asset handler:
            AZ::SerializeContext* serializeContext = nullptr;

            AZ::ComponentApplicationBus::BroadcastResult(serializeContext, &AZ::ComponentApplicationBus::Events::GetSerializeContext);
            if (serializeContext)
            {
                AZ::Data::Asset<AZ::Data::AssetData> asset = AZ::Data::AssetManager::Instance().GetAsset(productEntry->GetAssetId(), productEntry->GetAssetType(), false);
                AzToolsFramework::AssetEditor::AssetEditorRequestsBus::Broadcast(&AzToolsFramework::AssetEditor::AssetEditorRequests::OpenAssetEditor, asset);
            }

            foundHandler = true;
            break;
        }
    }

    if (!foundHandler)
    {
        // no generic asset handler for it.
        // try the system OS.
        bool openedSuccessfully = QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromUtf8(fullEntryPath.c_str())));
        if (!openedSuccessfully)
        {
            AZ_Printf("Asset Browser", "Unable to open '%s' using the operating system.  There might be no editor associated with this kind of file.\n", fullEntryPath.c_str());
            alreadyHandled = true;
            return;
        }
    }
}
