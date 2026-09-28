#pragma once

#include <QDomDocument>
#include <QXmlStreamReader>
#include <QDebug>
#include <QElapsedTimer>
#include <QDateTime>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/definitions/definition_indexes.h"
#include "src/backend/calibration/session/definition_catalogs.h"
#include "src/backend/config/config_session.h"
#include "src/backend/definition/definition_service.h"
#include "src/backend/ports/atomic_file_writer.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/file_system.h"
#include "src/backend/ports/resource_bundle.h"

#if defined(_WIN32) || defined(WIN32) || defined(_WIN64) || defined(WIN64)
#include <windows.h>
#else
#include <unistd.h>
#endif // Windows

class FileActions : public fastecu::calibration::IDefinitionCatalogs
{
  public:
    FileActions(fastecu::IFileSystem& file_system, fastecu::IResourceBundle& resource_bundle,
                fastecu::IFileRepository& file_repository, fastecu::IAtomicFileWriter& atomic_file_writer,
                fastecu::IEventSink& events, fastecu::config::ConfigSession& config);

    // Catalogs and startup source indexes used by the calibration session.
    fastecu::Result<fastecu::definition::DefinitionCatalog>
    catalog(fastecu::definition::DefinitionFormat format) override
    {
        return build_definition_catalog(format);
    }
    std::optional<std::string> indexed_source(fastecu::definition::DefinitionFormat format,
                                              std::string_view id) override
    {
        const QString source =
            definition_source(format, QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size())));
        return source.isEmpty() ? std::nullopt : std::optional<std::string>{source.toStdString()};
    }

    uint8_t float_precision = 15;
    // QString ecu_protocol;

    // The legacy EcuFlash/RomRaider definition index lists (see
    // definition_indexes.h's comment): derived catalog data FileActions
    // replaces from portable catalogs, not application configuration.
    using DefinitionIndexes = fastecu::definitions::DefinitionIndexes;
    DefinitionIndexes definitionIndexes;

    struct protocolsStructure
    {
        QStringList protocols;
        QStringList baudrate;
        QStringList databits;
        QStringList stopbits;
        QStringList parity;
        QStringList connect_timeout;
        QStringList send_timeout;
    } protocolsStruct;

    void create_romraider_def_id_list();
    void create_ecuflash_def_id_list();
    QString parse_hex_ecuid(uint8_t byte);

    /**************************************************
     * Parse negative response code message
     *************************************************/
    static QString parse_nrc_message(const QByteArray& nrc);
    /**************************************************
     * Parse diagnostic trouble code message
     *************************************************/
    static QString parse_dtc_message(uint16_t dtc);

    // Public so src/ui/desktop/definition's authoring dialog can reach the
    // portable submit seam; the interactive wizards that used to wrap them
    // moved there in step 6a-2.
    fastecu::Status submit_new_definition(std::string_view destination,
                                          const fastecu::definition::DefinitionHeaderInput&);
    fastecu::Status submit_imported_definition(std::string_view source, std::string_view destination,
                                               const fastecu::definition::DefinitionHeaderInput&);

  private:
    friend class TestFileActionsParsing;

    void remember_submitted_ecuflash_handle(std::string_view destination);
    fastecu::Result<fastecu::definition::DefinitionCatalog>
    build_definition_catalog(fastecu::definition::DefinitionFormat format);
    QString definition_source(fastecu::definition::DefinitionFormat format, const QString& id) const;
    void log_definition_error(const QString& operation, const fastecu::Error& error);
    // The composition's session, initialized before FileActions is built.
    fastecu::config::ConfigSession& configSession_;
    fastecu::IFileSystem& definitionFileSystem_;
    fastecu::definition::DefinitionService definitionService_;
    std::vector<std::string> submittedEcuflashHandles_;
    fastecu::IEventSink& events_;
};
