/*
# PostgreSQL Database Modeler (pgModeler)
#
# Copyright 2006-2013 - Raphael Araújo e Silva <rkhaotix@gmail.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation version 3.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# The complete text of GPLv3 is at LICENSE file on source code root directory.
# Also, you can get the complete GNU General Public License at <http://www.gnu.org/licenses/>
*/

#include "databaseimporthelper.h"

const QString DatabaseImportHelper::ARRAY_PATTERN="((\\[)[0-9]+(\\:)[0-9]+(\\])=)?(\\{)((.)+(,)*)*(\\})$";

DatabaseImportHelper::DatabaseImportHelper(QObject *parent) : QObject(parent)
{
	import_canceled=ignore_errors=import_sys_objs=import_ext_objs=false;
	auto_resolve_deps=true;
	import_filter=Catalog::LIST_ALL_OBJS | Catalog::EXCL_EXTENSION_OBJS | Catalog::EXCL_SYSTEM_OBJS;
	model_wgt=nullptr;
}

void DatabaseImportHelper::setConnection(Connection &conn)
{
	try
	{
		connection=conn;
		catalog.setConnection(conn);
	}
	catch(Exception &e)
	{
		throw Exception(e.getErrorMessage(), e.getErrorType(),__PRETTY_FUNCTION__,__FILE__,__LINE__, &e);
	}
}

void DatabaseImportHelper::setCurrentDatabase(const QString &dbname)
{
	try
	{
		connection.switchToDatabase(dbname);
		catalog.setConnection(connection);
	}
	catch(Exception &e)
	{
		throw Exception(e.getErrorMessage(), e.getErrorType(),__PRETTY_FUNCTION__,__FILE__,__LINE__, &e);
	}
}

void DatabaseImportHelper::setSelectedOIDs(ModelWidget *model_wgt, map<ObjectType, vector<unsigned>> &obj_oids, map<unsigned, vector<unsigned>> &col_oids)
{
	map<ObjectType, vector<unsigned> >::iterator itr=obj_oids.begin();

	if(!model_wgt)
		throw Exception(ERR_ASG_NOT_ALOC_OBJECT ,__PRETTY_FUNCTION__,__FILE__,__LINE__);

	this->model_wgt=model_wgt;
	dbmodel=model_wgt->getDatabaseModel();
	object_oids=obj_oids;
	column_oids=col_oids;

	//Fills the creation order vector with the selected OIDs
	creation_order.clear();
	while(itr!=obj_oids.end())
	{
		creation_order.insert(creation_order.end(), itr->second.begin(), itr->second.end());
		itr++;
	}

	//Sort the creation order vector to create the object in the correct sequence
	std::sort(creation_order.begin(), creation_order.end());

	user_objs.clear();
	system_objs.clear();
}

void DatabaseImportHelper::setImportOptions(bool import_sys_objs, bool import_ext_objs, bool auto_resolve_deps, bool ignore_errors, bool debug_mode)
{
	this->import_sys_objs=import_sys_objs;
	this->import_ext_objs=import_ext_objs;
	this->auto_resolve_deps=auto_resolve_deps;
	this->ignore_errors=ignore_errors;
	this->debug_mode=debug_mode;

	Connection::setPrintSQL(debug_mode);

	if(!import_sys_objs && import_ext_objs)
		import_filter=Catalog::LIST_ALL_OBJS | Catalog::EXCL_BUILTIN_ARRAY_TYPES | Catalog::EXCL_SYSTEM_OBJS;
	else if(import_sys_objs && !import_ext_objs)
		import_filter=Catalog::LIST_ALL_OBJS | Catalog::EXCL_BUILTIN_ARRAY_TYPES | Catalog::EXCL_EXTENSION_OBJS;
	else if(import_sys_objs && import_ext_objs)
		import_filter=Catalog::LIST_ALL_OBJS | Catalog::EXCL_BUILTIN_ARRAY_TYPES;
	else
		import_filter=Catalog::LIST_ALL_OBJS | Catalog::EXCL_BUILTIN_ARRAY_TYPES | Catalog::EXCL_EXTENSION_OBJS | Catalog::EXCL_SYSTEM_OBJS;
}

unsigned DatabaseImportHelper::getLastSystemOID(void)
{
	return(catalog.getLastSysObjectOID());
}

attribs_map DatabaseImportHelper::getObjects(ObjectType obj_type, const QString &schema, const QString &table, attribs_map extra_attribs)
{
	try
	{
		catalog.setFilter(import_filter);
		return(catalog.getObjectsNames(obj_type, schema, table, extra_attribs));
	}
	catch(Exception &e)
	{
		throw Exception(e.getErrorMessage(), e.getErrorType(),__PRETTY_FUNCTION__,__FILE__,__LINE__, &e);
	}
}

void DatabaseImportHelper::swapSequencesTablesIds(void)
{
	//Schema *schema=nullptr;
	BaseObject *table=nullptr, *sequence=nullptr;
	map<QString, QString>::iterator itr;

	//Swapping the id's between sequences and tables to avoid reference breaking on SQL code
	itr=seq_tab_swap.begin();
	while(itr!=seq_tab_swap.end())
	{
		sequence=dbmodel->getObject(getObjectName(itr->first), OBJ_SEQUENCE);
		table=dbmodel->getObject(getObjectName(itr->second), OBJ_TABLE);
		if(sequence && table)
			BaseObject::swapObjectsIds(sequence, table, false);
		itr++;
	}
}

void DatabaseImportHelper::retrieveSystemObjects(void)
{
	int progress=0;
	vector<attribs_map>::iterator itr;
	map<unsigned, attribs_map> *obj_map=nullptr;
	vector<attribs_map> objects;
	ObjectType sys_objs[]={ OBJ_SCHEMA, OBJ_ROLE, OBJ_TABLESPACE,
													OBJ_LANGUAGE, /* OBJ_COLLATION,*/ OBJ_TYPE };
	unsigned i=0, oid=0, cnt=sizeof(sys_objs)/sizeof(ObjectType);

	for(i=0; i < cnt && !import_canceled; i++)
	{
		emit s_progressUpdated(progress,
													 trUtf8("Retrieving system objects... `%1'").arg(BaseObject::getTypeName(sys_objs[i])),
													 sys_objs[i]);

		if(sys_objs[i]!=OBJ_TYPE)
		{
			obj_map=&system_objs;

			if(sys_objs[i]!=OBJ_LANGUAGE)
				catalog.setFilter(Catalog::LIST_ONLY_SYS_OBJS);
			else
				catalog.setFilter(Catalog::LIST_ALL_OBJS);
		}
		else
		{
			obj_map=&types;
			catalog.setFilter(Catalog::LIST_ALL_OBJS);
		}

		//Query the objects on the catalog and put them on the map
		objects=catalog.getObjectsAttributes(sys_objs[i]);
		itr=objects.begin();

		while(itr!=objects.end() && !import_canceled)
		{
			oid=itr->at(ParsersAttributes::OID).toUInt();
			(*obj_map)[oid]=(*itr);
			itr++;
		}

		progress=(i/static_cast<float>(cnt))*10;
		sleepThread(5);
	}
}

void DatabaseImportHelper::retrieveUserObjects(void)
{
	int progress=0;
	map<ObjectType, vector<unsigned>>::iterator oid_itr=object_oids.begin();
	vector<attribs_map>::iterator itr;
	vector<attribs_map> objects;
	unsigned i=0, oid=0;
	map<unsigned, vector<unsigned>>::iterator col_itr;
	QStringList names;

	i=0;
	catalog.setFilter(import_filter);

	//Retrieving selected database level objects and table children objects (except columns)
	while(oid_itr!=object_oids.end() && !import_canceled)
	{
		emit s_progressUpdated(progress,
													 trUtf8("Retrieving objects... `%1'").arg(BaseObject::getTypeName(oid_itr->first)),
													 oid_itr->first);

		objects=catalog.getObjectsAttributes(oid_itr->first, "", "", oid_itr->second);
		itr=objects.begin();

		while(itr!=objects.end() && !import_canceled)
		{
			oid=itr->at(ParsersAttributes::OID).toUInt();
			user_objs[oid]=(*itr);
			itr++;
		}

		objects.clear();
		progress=(i/static_cast<float>(object_oids.size()))*100;
		oid_itr++; i++;
		sleepThread(5);
	}

	//Retrieving all selected table columns
	i=0;
	col_itr=column_oids.begin();
	while(col_itr!=column_oids.end())
	{
		emit s_progressUpdated(progress,
													 trUtf8("Retrieving objects... `%1'").arg(BaseObject::getTypeName(OBJ_COLUMN)),
													 OBJ_COLUMN);

		names=getObjectName(QString::number(col_itr->first)).split(".");
		objects=catalog.getObjectsAttributes(OBJ_COLUMN, names[0], names[1], col_itr->second);
		itr=objects.begin();

		while(itr!=objects.end() && !import_canceled)
		{
			oid=itr->at(ParsersAttributes::OID).toUInt();
			columns[col_itr->first][oid]=(*itr);
			itr++;
		}

		objects.clear();
		progress=(i/static_cast<float>(column_oids.size()))*100;
		col_itr++; i++;
		sleepThread(5);
	}
}

void DatabaseImportHelper::createObjects(void)
{
	int progress=0;
	attribs_map attribs;
	ObjectType obj_type;
	unsigned i=0, oid=0;

	for(i=0; i < creation_order.size() && !import_canceled; i++)
	{
		oid=creation_order[i];
		attribs=user_objs[oid];
		obj_type=static_cast<ObjectType>(attribs[ParsersAttributes::OBJECT_TYPE].toUInt());

		try
		{
			/* Constraints are ignored in these phase being pushed into an auxiliary list
				 in order to be created later */
			if(obj_type!=OBJ_CONSTRAINT)
			{
				emit s_progressUpdated(progress,
															 trUtf8("Creating object `%1' `(%2)'...")
															 .arg(attribs[ParsersAttributes::NAME])
															 .arg(BaseObject::getTypeName(obj_type)),
															 obj_type);

				createObject(attribs);
			}
			else
				constr_creation_order.push_back(oid);
		}
		catch(Exception &e)
		{
			if(ignore_errors)
				errors.push_back(e);
			else
				throw Exception(e.getErrorMessage(), e.getErrorType(),__PRETTY_FUNCTION__,__FILE__,__LINE__, &e);
		}

		progress=(i/static_cast<float>(creation_order.size())) * 100;
		sleepThread(5);
	}

	//Creating table inheiritances
	if(dbmodel->getObjectCount(OBJ_TABLE) > 0 && !import_canceled)
	{
		emit s_progressUpdated(100,
													 trUtf8("Creating table inheritances..."),
													 OBJ_RELATIONSHIP);
		try
		{
			createTableInheritances();
		}
		catch(Exception &e)
		{
			if(ignore_errors)
				errors.push_back(e);
			else
				throw Exception(e.getErrorMessage(), e.getErrorType(),__PRETTY_FUNCTION__,__FILE__,__LINE__, &e);
		}
	}
}

void DatabaseImportHelper::createConstraints(void)
{
	int progress=0;
	attribs_map attribs;
	unsigned i=0, oid=0;

	for(i=0; i < constr_creation_order.size() && !import_canceled; i++)
	{
		oid=constr_creation_order[i];
		attribs=user_objs[oid];

		try
		{
			emit s_progressUpdated(progress,
														 trUtf8("Creating object `%1' `(%2)'...")
														 .arg(attribs[ParsersAttributes::NAME])
														 .arg(BaseObject::getTypeName(OBJ_CONSTRAINT)),
														 OBJ_CONSTRAINT);

			createObject(attribs);
		}
		catch(Exception &e)
		{
			if(ignore_errors)
				errors.push_back(e);
			else
				throw Exception(e.getErrorMessage(), e.getErrorType(),__PRETTY_FUNCTION__,__FILE__,__LINE__, &e);
		}

		progress=(i/static_cast<float>(constr_creation_order.size())) * 100;
		sleepThread(10);
	}
}

void DatabaseImportHelper::createPermissions(void)
{
	try
	{
		unsigned i=0, progress=0;
		vector<unsigned>::iterator itr, itr_obj=obj_perms.begin();
		map<unsigned, vector<unsigned>>::iterator itr_cols=col_perms.begin();

		//Create the object level permission
		while(itr_obj!=obj_perms.end() && !import_canceled)
		{
			emit s_progressUpdated(progress, trUtf8("Creating objects permissions..."), OBJ_PERMISSION);

			createPermission(user_objs[*itr_obj]);
			itr_obj++;

			progress=((i++)/static_cast<float>(obj_perms.size())) * 100;
			sleepThread(10);
		}

		//Create the column level permission
		i=0;
		while(itr_cols!=col_perms.end() && !import_canceled)
		{
			emit s_progressUpdated(progress, trUtf8("Creating columns permissions..."), OBJ_PERMISSION);

			itr=col_perms[itr_cols->first].begin();

			while(itr!=itr_cols->second.end())
			{
				createPermission(columns[itr_cols->first][*itr]);
				itr++;
			}

			itr_cols++;
			progress=((i++)/static_cast<float>(col_perms.size())) * 100;
			sleepThread(10);
		}

	}
	catch(Exception &e)
	{
		if(ignore_errors)
			errors.push_back(e);
		else
			throw Exception(e.getErrorMessage(), e.getErrorType(),__PRETTY_FUNCTION__,__FILE__,__LINE__, &e);
	}
}

void DatabaseImportHelper::updateFKRelationships(void)
{
	int progress=0;
	vector<BaseObject *>::iterator itr_tab, itr_tab_end;
	unsigned i=0, count=0;
	Table *tab=nullptr;

	itr_tab=dbmodel->getObjectList(OBJ_TABLE)->begin();
	itr_tab_end=dbmodel->getObjectList(OBJ_TABLE)->end();
	count=dbmodel->getObjectList(OBJ_TABLE)->size();
	i=0;

	try
	{
		while(itr_tab!=itr_tab_end && !import_canceled)
		{
			tab=dynamic_cast<Table *>(*itr_tab);

			emit s_progressUpdated(progress,
														 trUtf8("Update relationships of `%1' `(%2)'...")
														 .arg(tab->getName())
														 .arg(BaseObject::getTypeName(OBJ_TABLE)),
														 OBJ_TABLE);

			dbmodel->updateTableFKRelationships(tab);

			progress=(i/static_cast<float>(count)) * 100;
			itr_tab++; i++;
			sleepThread(10);
		}
	}
	catch(Exception &e)
	{
		throw Exception(e.getErrorMessage(), e.getErrorType(),__PRETTY_FUNCTION__,__FILE__,__LINE__, &e);
	}
}

void DatabaseImportHelper::sleepThread(unsigned msecs)
{
	if(qApp->thread()!=this->thread())
		QThread::msleep(msecs);
}

void DatabaseImportHelper::importDatabase(void)
{
	try
	{
		retrieveSystemObjects();
		retrieveUserObjects();
		createObjects();
		createConstraints();
		createPermissions();
		updateFKRelationships();

		if(!import_canceled)
		{
			swapSequencesTablesIds();

			if(!errors.empty())
			{
				QString log_name;

				//Writing the erros to log file
				log_name=GlobalAttributes::TEMPORARY_DIR +
								 GlobalAttributes::DIR_SEPARATOR +
								 QString("%1_%2_%3.log").arg(dbmodel->getName())
																				.arg("import")
																				.arg(QDateTime::currentDateTime().toString("yyyy-MM-dd_hhmmss"));

				import_log.setFileName(log_name);
				import_log.open(QFile::WriteOnly);

				for(unsigned i=0; i < errors.size() && import_log.isOpen(); i++)
					import_log.write(errors[i].getExceptionsText().toStdString().c_str());

				import_log.close();

				emit s_importFinished(Exception(trUtf8("The database import ended but some errors were generated and saved into the log file `%1'. This file will last until pgModeler quit.").arg(log_name),
																				__PRETTY_FUNCTION__,__FILE__,__LINE__));
			}
			else
				emit s_importFinished();
		}
		else
			emit s_importCanceled();

		resetImportParameters();

		sleepThread(20);
	}
	catch(Exception &e)
	{
		resetImportParameters();

		/* When running in a separated thread (other than the main application thread)
		redirects the error in form of signal */
		if(this->thread() && this->thread()!=qApp->thread())
			emit s_importAborted(Exception(e.getErrorMessage(), e.getErrorType(),__PRETTY_FUNCTION__,__FILE__,__LINE__, &e, e.getExtraInfo()));
		else
			//Redirects any error to the user
			throw Exception(e.getErrorMessage(),e.getErrorType(),__PRETTY_FUNCTION__,__FILE__,__LINE__, &e, e.getExtraInfo());
	}
}

void DatabaseImportHelper::cancelImport(void)
{
	import_canceled=true;
}

void DatabaseImportHelper::createObject(attribs_map &attribs)
{
	unsigned oid=attribs[ParsersAttributes::OID].toUInt();
	ObjectType obj_type=static_cast<ObjectType>(attribs[ParsersAttributes::OBJECT_TYPE].toUInt());
	QString obj_name=getObjectName(attribs[ParsersAttributes::OID], (obj_type==OBJ_FUNCTION || obj_type==OBJ_OPERATOR));

	try
	{	
		if(!import_canceled &&
			 (obj_type==OBJ_DATABASE || TableObject::isTableObject(obj_type) ||

			 //If the object does not exists on both model and created objects vector
			 ((std::find(created_objs.begin(), created_objs.end(), oid)==created_objs.end()) &&
				 dbmodel->getObjectIndex(obj_name, obj_type) < 0)))
		{
			if(TableObject::isTableObject(obj_type))
				attribs[ParsersAttributes::DECL_IN_TABLE]="";

			//System objects will have the sql disabled by default
			attribs[ParsersAttributes::SQL_DISABLED]=(oid > catalog.getLastSysObjectOID() ? "" : "1");
			attribs[ParsersAttributes::COMMENT]=getComment(attribs);

			if(attribs.count(ParsersAttributes::OWNER))
				attribs[ParsersAttributes::OWNER]=getDependencyObject(attribs[ParsersAttributes::OWNER], OBJ_ROLE);

			if(attribs.count(ParsersAttributes::TABLESPACE))
				attribs[ParsersAttributes::TABLESPACE]=getDependencyObject(attribs[ParsersAttributes::TABLESPACE], OBJ_TABLESPACE);

			if(attribs.count(ParsersAttributes::SCHEMA))
				attribs[ParsersAttributes::SCHEMA]=getDependencyObject(attribs[ParsersAttributes::SCHEMA], OBJ_SCHEMA);

			if(!attribs[ParsersAttributes::PERMISSION].isEmpty())
				obj_perms.push_back(oid);

			switch(obj_type)
			{
				case OBJ_DATABASE: configureDatabase(attribs); break;
				case OBJ_TABLESPACE: createTablespace(attribs); break;
				case OBJ_SCHEMA: createSchema(attribs); break;
				case OBJ_ROLE: createRole(attribs); break;
				case OBJ_DOMAIN: createDomain(attribs); break;
				case OBJ_EXTENSION: createExtension(attribs); break;
				case OBJ_FUNCTION: createFunction(attribs); break;
				case OBJ_LANGUAGE: createLanguage(attribs); break;
				case OBJ_OPFAMILY: createOperatorFamily(attribs); break;
				case OBJ_OPCLASS: createOperatorClass(attribs); break;
				case OBJ_OPERATOR: createOperator(attribs); break;
				case OBJ_COLLATION: createCollation(attribs); break;
				case OBJ_CAST: createCast(attribs); break;
				case OBJ_CONVERSION: createConversion(attribs); break;
				case OBJ_SEQUENCE: createSequence(attribs); break;
				case OBJ_AGGREGATE: createAggregate(attribs); break;
				case OBJ_TYPE: createType(attribs); break;
				case OBJ_TABLE: createTable(attribs); break;
				case OBJ_VIEW: createView(attribs); break;
				case OBJ_RULE: createRule(attribs); break;
				case OBJ_TRIGGER: createTrigger(attribs); break;
				case OBJ_INDEX: createIndex(attribs); break;
				case OBJ_CONSTRAINT: createConstraint(attribs); break;

				default:
					qDebug(QString("create method for %1 isn't implemented!").arg(BaseObject::getSchemaName(obj_type)).toStdString().c_str());
				break;
			}

			/* Register the object oid on the list of created objects to avoid creating it again
				 on recursive object creation. (see getDependencyObject()) */
			created_objs.push_back(oid);
		}
	}
	catch(Exception &e)
	{
		throw Exception(Exception::getErrorMessage(ERR_OBJECT_NOT_IMPORTED)
										.arg(obj_name).arg(BaseObject::getTypeName(obj_type)),
										ERR_OBJECT_NOT_IMPORTED,__PRETTY_FUNCTION__,__FILE__,__LINE__, &e);
	}
}

QString DatabaseImportHelper::getComment(attribs_map &attribs)
{
	try
	{
		QString xml_def;

		if(!attribs[ParsersAttributes::COMMENT].isEmpty())
			xml_def=SchemaParser::getCodeDefinition(ParsersAttributes::COMMENT, attribs, SchemaParser::XML_DEFINITION);

		return(xml_def);
	}
	catch(Exception &e)
	{
		throw Exception(e.getErrorMessage(), e.getErrorType(),__PRETTY_FUNCTION__,__FILE__,__LINE__, &e);
	}
}

QString DatabaseImportHelper::getDependencyObject(const QString &oid, ObjectType obj_type, bool use_signature, bool recursive_dep_res, bool generate_xml, attribs_map extra_attribs)
{
	try
	{
		QString xml_def;
		unsigned obj_oid=oid.toUInt();

		if(obj_oid > 0)
		{
			attribs_map obj_attr;
			attribs_map::iterator itr=extra_attribs.begin();

			if(system_objs.count(obj_oid))
				obj_attr=system_objs[obj_oid];
			else
				obj_attr=user_objs[obj_oid];

			/* If the attributes for the dependency does not exists and the automatic dependency
			resolution is enable, the object's attributes will be retrieved from catalog */
			if(auto_resolve_deps && obj_attr.empty() &&
				 ((import_ext_objs && catalog.isExtensionObject(obj_oid)) ||
					(!import_sys_objs && !import_ext_objs &&
						obj_oid > catalog.getLastSysObjectOID() && !catalog.isExtensionObject(obj_oid)) ||
					(import_sys_objs  && obj_oid <= catalog.getLastSysObjectOID())))
			{
				catalog.setFilter(Catalog::LIST_ALL_OBJS);
				vector<attribs_map> attribs_vect=catalog.getObjectsAttributes(obj_type,"","", { obj_oid });

				if(!attribs_vect.empty())
				{
					if(obj_oid <= catalog.getLastSysObjectOID())
						system_objs[obj_oid]=attribs_vect[0];
					else
						user_objs[obj_oid]=attribs_vect[0];

					obj_attr=attribs_vect[0];
				}
			}

			if(!obj_attr.empty())
			{
				QString obj_name;

				while(itr!=extra_attribs.end())
				{
					obj_attr[itr->first]=itr->second;
					itr++;
				}

				/* If the attributes of the dependency exists but it was not created on the model yet,
					 pgModeler will create it and it's dependencies recursively */
				if(recursive_dep_res && !TableObject::isTableObject(obj_type) &&
					 obj_type!=OBJ_DATABASE && dbmodel->getObjectIndex(obj_name, obj_type) < 0)
					createObject(obj_attr);

				if(use_signature)
					obj_name=obj_attr[ParsersAttributes::SIGNATURE]=getObjectName(oid, true);
				else
					obj_name=obj_attr[ParsersAttributes::NAME]=getObjectName(oid);

				if(generate_xml)
				{
					obj_attr[ParsersAttributes::REDUCED_FORM]="1";
					SchemaParser::setIgnoreUnkownAttributes(true);
					xml_def=SchemaParser::getCodeDefinition(BaseObject::getSchemaName(obj_type), obj_attr, SchemaParser::XML_DEFINITION);
					SchemaParser::setIgnoreUnkownAttributes(false);
				}
				else
					xml_def=obj_name;
			}
			else
				/* If the object oid is valid but there is no attribute set to it creates a xml definition
					 containing an alert indicating that the object is unknown */
				xml_def=QString("\t<!--[ unknown object OID=%1 ]-->\n").arg(oid);
		}

		return(xml_def);
	}
	catch(Exception &e)
	{
		throw Exception(e.getErrorMessage(), e.getErrorType(),__PRETTY_FUNCTION__,__FILE__,__LINE__, &e);
	}
}

void DatabaseImportHelper::loadObjectXML(ObjectType obj_type, attribs_map &attribs)
{
	QString xml_buf;

	try
	{
		SchemaParser::setIgnoreUnkownAttributes(true);
		xml_buf=SchemaParser::getCodeDefinition(BaseObject::getSchemaName(obj_type), attribs, SchemaParser::XML_DEFINITION);

		SchemaParser::setIgnoreUnkownAttributes(false);
		XMLParser::restartParser();

		if(debug_mode)
		{
			QTextStream ts(stdout);
			ts << xml_buf << endl;
		}

		XMLParser::loadXMLBuffer(xml_buf);
	}
	catch(Exception &e)
	{
		throw Exception(e.getErrorMessage(), e.getErrorType(),__PRETTY_FUNCTION__,__FILE__,__LINE__, &e, xml_buf);
	}
}

void DatabaseImportHelper::resetImportParameters(void)
{
	Connection::setPrintSQL(false);
	import_canceled=false;
	dbmodel=nullptr;
	model_wgt=nullptr;
	column_oids.clear();
	object_oids.clear();
	types.clear();
	user_objs.clear();
	creation_order.clear();
	seq_tab_swap.clear();
	columns.clear();
	system_objs.clear();
	created_objs.clear();
	errors.clear();
	constr_creation_order.clear();
	obj_perms.clear();
	col_perms.clear();
}


void DatabaseImportHelper::createTablespace(attribs_map &attribs)
{
	Tablespace *tabspc=nullptr;

	try
	{
		loadObjectXML(OBJ_TABLESPACE, attribs);
		tabspc=dbmodel->createTablespace();
		dbmodel->addObject(tabspc);
	}
	catch(Exception &e)
	{
		if(tabspc) delete(tabspc);
		throw Exception(e.getErrorMessage(), e.getErrorType(),__PRETTY_FUNCTION__,__FILE__,__LINE__, &e);
	}
}

void DatabaseImportHelper::createSchema(attribs_map &attribs)
{
	Schema *schema=nullptr;

	try
	{
		attribs[ParsersAttributes::RECT_VISIBLE]="";
		attribs[ParsersAttributes::FILL_COLOR]=QColor(rand() % 255, rand() % 255, rand() % 255).name();
		loadObjectXML(OBJ_SCHEMA, attribs);

		schema=dbmodel->createSchema();
		dbmodel->addObject(schema);
	}
	catch(Exception &e)
	{
		if(schema) delete(schema);
		throw Exception(e.getErrorMessage(), e.getErrorType(),
										__PRETTY_FUNCTION__,__FILE__,__LINE__, &e, XMLParser::getXMLBuffer());
	}
}

void DatabaseImportHelper::createRole(attribs_map &attribs)
{
	Role *role=nullptr;

	try
	{
		QString role_types[]={ ParsersAttributes::REF_ROLES,
													 ParsersAttributes::ADMIN_ROLES,
													 ParsersAttributes::MEMBER_ROLES };

		for(unsigned i=0; i < 3; i++)
			attribs[role_types[i]]=getObjectNames(attribs[role_types[i]]).join(',');

		loadObjectXML(OBJ_ROLE, attribs);
		role=dbmodel->createRole();
		dbmodel->addObject(role);
	}
	catch(Exception &e)
	{
		if(role) delete(role);
		throw Exception(e.getErrorMessage(), e.getErrorType(),
										__PRETTY_FUNCTION__,__FILE__,__LINE__, &e, XMLParser::getXMLBuffer());
	}
}

void DatabaseImportHelper::createDomain(attribs_map &attribs)
{
	Domain *dom=nullptr;

	try
	{
		attribs[ParsersAttributes::TYPE]=getType(attribs[ParsersAttributes::TYPE], true, attribs);
		attribs[ParsersAttributes::COLLATION]=getDependencyObject(attribs[ParsersAttributes::COLLATION], OBJ_COLLATION);
		loadObjectXML(OBJ_DOMAIN, attribs);
		dom=dbmodel->createDomain();
		dbmodel->addDomain(dom);
	}
	catch(Exception &e)
	{
		if(dom) delete(dom);
		throw Exception(e.getErrorMessage(), e.getErrorType(),
										__PRETTY_FUNCTION__,__FILE__,__LINE__, &e, XMLParser::getXMLBuffer());
	}
}

void DatabaseImportHelper::createExtension(attribs_map &attribs)
{
	Extension *ext=nullptr;

	try
	{
		loadObjectXML(OBJ_EXTENSION, attribs);
		ext=dbmodel->createExtension();
		dbmodel->addExtension(ext);
	}
	catch(Exception &e)
	{
		if(ext) delete(ext);
		throw Exception(e.getErrorMessage(), e.getErrorType(),
										__PRETTY_FUNCTION__,__FILE__,__LINE__, &e, XMLParser::getXMLBuffer());
	}
}

QStringList DatabaseImportHelper::parseDefaultValues(const QString &def_vals, const QString &str_delim, const QString &val_sep)
{
	int idx=0, delim_start, delim_end, sep_idx, pos=0;
	QStringList values;

	while(idx < def_vals.size())
	{
		//Get the index of string delimiters (default: ')
		delim_start=def_vals.indexOf(str_delim, idx);
		delim_end=def_vals.indexOf(str_delim, delim_start + 1);

		/* Get the index of value separator on default value string
			 (by default the pg_get_expr separates values by comma and space (, ) */
		sep_idx=def_vals.indexOf(val_sep, idx);

		/* If there is no separator on string (only one value) or the is
			 beyond the string delimiters or even there is no string delimiter on string */
		if(sep_idx < 0 ||
			 (sep_idx >=0 && delim_start >= 0 && delim_end >= 0 &&
				(sep_idx < delim_start || sep_idx > delim_end)) ||
			 (sep_idx >=0 && (delim_start < 0 || delim_end < 0)))
		{
			//Extract the value from the current position
			values.push_back(def_vals.mid(pos, sep_idx-pos).trimmed());

			//If there is no separator on string indicates that it contains only one value
			if(sep_idx < 0)
				//Forcing the loop abort
				idx=def_vals.size();
			else
			{
				//Passing to the next value right after the separator
				pos=sep_idx+1;
				idx=pos;
			}
		}
		/* If the separator is between a string delimitation e.g.'abc, def' it will be ignored
		and the current postion will be moved to the first char after string delimiter */
		else if(delim_start>=0 && delim_end >= 0 &&
						sep_idx >= delim_start && sep_idx <=delim_end)
		{
			idx=delim_end+1;

			/* If the index reaches the end of string but the cursor (pos) isn't at end
			indicates that the last values wasn't retrieved, this way, the value will be
			pushed to list of values */
			if(idx >= def_vals.size() && pos < def_vals.size())
				values.push_back(def_vals.mid(pos, def_vals.size()));
		}
		else
			idx++;
	}

	return(values);
}

void DatabaseImportHelper::createFunction(attribs_map &attribs)
{
	Function *func=nullptr;
	Parameter param;
	PgSQLType type;
	unsigned dim=0;
	QStringList param_types, param_names, param_modes, param_def_vals;
	int def_val_idx=0;

	try
	{
		param_types=getTypes(attribs[ParsersAttributes::ARG_TYPES], false);
		param_names=parseArrayValues(attribs[ParsersAttributes::ARG_NAMES]);
		param_modes=parseArrayValues(attribs[ParsersAttributes::ARG_MODES]);
		param_def_vals=parseDefaultValues(attribs[ParsersAttributes::ARG_DEFAULTS]);

		for(int i=0; i < param_types.size(); i++)
		{
			/* If the function is to be used as a user-defined data type support functions
				 the parameter type will be renamed to "any" (see rules on Type::setFunction()) */
			if(i==0 &&
				 (attribs[ParsersAttributes::REF_TYPE]==ParsersAttributes::SEND_FUNC ||
					attribs[ParsersAttributes::REF_TYPE]==ParsersAttributes::OUTPUT_FUNC ||
					attribs[ParsersAttributes::REF_TYPE]==ParsersAttributes::CANONICAL_FUNC))
				type=PgSQLType("any");
			else
			{
				//If the type contains array descriptor [] set the dimension to 1
				dim=(param_types[i].contains("[]") ? 1 : 0);

				//Create the type
				param_types[i].remove("[]");
				type=PgSQLType(param_types[i]);
				type.setDimension(dim);
			}

			//Alocates a new parameter
			param=Parameter();
			param.setType(type);

			param.setIn(true);

			if(param_names.isEmpty())
				param.setName(QString("_param%1").arg(i+1));
			else
				param.setName(param_names[i]);

			//Parameter modes: i = IN, o = OUT, b = INOUT, v = VARIADIC
			if(!param_modes.isEmpty())
			{
				param.setIn(param_modes[i]=="i" || param_modes[i]=="b");
				param.setOut(param_modes[i]=="o" || param_modes[i]=="b");
				param.setVariadic(param_modes[i]=="v");
			}

			//Setting the default value for the current paramenter. OUT parameter doesn't receive default values.
			if(def_val_idx < param_def_vals.size() && (!param.isOut() || (param.isIn() && param.isOut())))
				param.setDefaultValue(param_def_vals[def_val_idx++]);

			//If the mode is 't' indicates that the current parameter will be used as a return table colum
			if(!param_modes.isEmpty() && param_modes[i]=="t")
				attribs[ParsersAttributes::RETURN_TABLE]+=param.getCodeDefinition(SchemaParser::XML_DEFINITION);
			else
				attribs[ParsersAttributes::PARAMETERS]+=param.getCodeDefinition(SchemaParser::XML_DEFINITION);
		}

		//Case the function's language is C the symbol is the 'definition' attribute
		if(getObjectName(attribs[ParsersAttributes::LANGUAGE])==~LanguageType("c"))
		{
			attribs[ParsersAttributes::SYMBOL]=attribs[ParsersAttributes::DEFINITION];
			attribs[ParsersAttributes::DEFINITION]="";
		}

		//Get the language reference code
		attribs[ParsersAttributes::LANGUAGE]=getDependencyObject(attribs[ParsersAttributes::LANGUAGE], OBJ_LANGUAGE);

		//Get the return type if there is no return table configured
		if(attribs[ParsersAttributes::RETURN_TABLE].isEmpty())
		{
			/* If the function is to be used as a user-defined data type support functions
				 the return type will be renamed to "any" (see rules on Type::setFunction()) */
			if(attribs[ParsersAttributes::REF_TYPE]==ParsersAttributes::INPUT_FUNC ||
				 attribs[ParsersAttributes::REF_TYPE]==ParsersAttributes::RECV_FUNC ||
				 attribs[ParsersAttributes::REF_TYPE]==ParsersAttributes::CANONICAL_FUNC)
				attribs[ParsersAttributes::RETURN_TYPE]=PgSQLType("any").getCodeDefinition(SchemaParser::XML_DEFINITION);
			else
				attribs[ParsersAttributes::RETURN_TYPE]=getType(attribs[ParsersAttributes::RETURN_TYPE], true);
		}

		loadObjectXML(OBJ_FUNCTION, attribs);
		func=dbmodel->createFunction();
		dbmodel->addFunction(func);
	}
	catch(Exception &e)
	{
		if(func) delete(func);
		throw Exception(e.getErrorMessage(), e.getErrorType(),
										__PRETTY_FUNCTION__,__FILE__,__LINE__, &e, XMLParser::getXMLBuffer());
	}
}

void DatabaseImportHelper::createLanguage(attribs_map &attribs)
{
	Language *lang=nullptr;

	try
	{
		unsigned lang_oid, func_oid;
		QString func_types[]={ ParsersAttributes::VALIDATOR_FUNC,
													 ParsersAttributes::HANDLER_FUNC,
													 ParsersAttributes::INLINE_FUNC };

		lang_oid=attribs[ParsersAttributes::OID].toUInt();
		for(unsigned i=0; i < 3; i++)
		{
			func_oid=attribs[func_types[i]].toUInt();

			/* Workaround: in case of importing system languages like "internal" where the validator/handler
				 function is defined after the language pgModeler will raise errors so in order to continue
				 the import these fuctions are simply ignored */
			if(func_oid < lang_oid)
				attribs[func_types[i]]=getDependencyObject(attribs[func_types[i]], OBJ_FUNCTION, true , true, true, {{ParsersAttributes::REF_TYPE, func_types[i]}});
			else
				attribs[func_types[i]]="";
		}

		loadObjectXML(OBJ_LANGUAGE, attribs);
		lang=dbmodel->createLanguage();
		dbmodel->addLanguage(lang);
	}
	catch(Exception &e)
	{
		if(lang) delete(lang);
		throw Exception(e.getErrorMessage(), e.getErrorType(),
										__PRETTY_FUNCTION__,__FILE__,__LINE__, &e, XMLParser::getXMLBuffer());
	}
}

void DatabaseImportHelper::createOperatorFamily(attribs_map &attribs)
{
	OperatorFamily *opfam=nullptr;

	try
	{
		loadObjectXML(OBJ_OPFAMILY, attribs);
		opfam=dbmodel->createOperatorFamily();
		dbmodel->addOperatorFamily(opfam);
	}
	catch(Exception &e)
	{
		if(opfam) delete(opfam);
		throw Exception(e.getErrorMessage(), e.getErrorType(),
										__PRETTY_FUNCTION__,__FILE__,__LINE__, &e, XMLParser::getXMLBuffer());
	}
}

void DatabaseImportHelper::createOperatorClass(attribs_map &attribs)
{
	OperatorClass *opclass=nullptr;

	try
	{
		attribs_map elem_attr;
		vector<attribs_map> elems;
		QStringList array_vals, list;

		attribs[ParsersAttributes::FAMILY]=getObjectName(attribs[ParsersAttributes::FAMILY]);
		attribs[ParsersAttributes::TYPE]=getType(attribs[ParsersAttributes::TYPE], true, attribs);

		//Generating attributes for STORAGE elements
		if(attribs[ParsersAttributes::STORAGE]!="0")
		{
			elem_attr[ParsersAttributes::STORAGE]="1";
			elem_attr[ParsersAttributes::DEFINITION]=getType(attribs[ParsersAttributes::STORAGE], true);
			elems.push_back(elem_attr);
		}
		else if(attribs[ParsersAttributes::FUNCTION].isEmpty() &&
						attribs[ParsersAttributes::OPERATOR].isEmpty())
		{
			elem_attr[ParsersAttributes::STORAGE]="1";
			elem_attr[ParsersAttributes::DEFINITION]=attribs[ParsersAttributes::TYPE];
			elems.push_back(elem_attr);
		}

		//Generating attributes for FUNCTION elements
		if(!attribs[ParsersAttributes::FUNCTION].isEmpty())
		{
			elem_attr.clear();
			elem_attr[ParsersAttributes::FUNCTION]="1";
			array_vals=parseArrayValues(attribs[ParsersAttributes::FUNCTION]);

			for(int i=0; i < array_vals.size(); i++)
			{
				list=array_vals[i].split(':');
				elem_attr[ParsersAttributes::STRATEGY_NUM]=list[0];
				elem_attr[ParsersAttributes::DEFINITION]=getDependencyObject(list[1], OBJ_FUNCTION, true);
				elems.push_back(elem_attr);
			}
		}

		//Generating attributes for OPERATOR elements
		if(!attribs[ParsersAttributes::OPERATOR].isEmpty())
		{
			elem_attr.clear();
			elem_attr[ParsersAttributes::OPERATOR]="1";
			array_vals=parseArrayValues(attribs[ParsersAttributes::OPERATOR]);

			for(int i=0; i < array_vals.size(); i++)
			{
				list=array_vals[i].split(':');
				elem_attr[ParsersAttributes::STRATEGY_NUM]=list[0];
				elem_attr[ParsersAttributes::DEFINITION]+=getDependencyObject(list[1], OBJ_OPERATOR, true);
				elem_attr[ParsersAttributes::DEFINITION]+=getDependencyObject(list[2], OBJ_OPERATOR, true);
				elems.push_back(elem_attr);
			}
		}

		//Generating the complete XML code for operator class elements
		for(unsigned i=0; i < elems.size(); i++)
		{
			SchemaParser::setIgnoreUnkownAttributes(true);
			attribs[ParsersAttributes::ELEMENTS]+=SchemaParser::getCodeDefinition(ParsersAttributes::ELEMENT, elems[i], SchemaParser::XML_DEFINITION);
			SchemaParser::setIgnoreUnkownAttributes(false);
		}

		loadObjectXML(OBJ_OPCLASS, attribs);
		opclass=dbmodel->createOperatorClass();
		dbmodel->addOperatorClass(opclass);
	}
	catch(Exception &e)
	{
		if(opclass) delete(opclass);
		throw Exception(e.getErrorMessage(), e.getErrorType(),
										__PRETTY_FUNCTION__,__FILE__,__LINE__, &e, XMLParser::getXMLBuffer());
	}
}

void DatabaseImportHelper::createOperator(attribs_map &attribs)
{
	Operator *oper=nullptr;

	try
	{
		int pos;
		QRegExp regexp;
		QString op_signature,

						func_types[]={ ParsersAttributes::OPERATOR_FUNC,
													 ParsersAttributes::RESTRICTION_FUNC,
													 ParsersAttributes::JOIN_FUNC },

						arg_types[]= { ParsersAttributes::LEFT_TYPE,
													 ParsersAttributes::RIGHT_TYPE },

						op_types[]=  { ParsersAttributes::COMMUTATOR_OP,
													 ParsersAttributes::NEGATOR_OP };

		for(unsigned i=0; i < 3; i++)
			attribs[func_types[i]]=getDependencyObject(attribs[func_types[i]], OBJ_FUNCTION, true, true, true, {{ParsersAttributes::REF_TYPE, func_types[i]}});

		for(unsigned i=0; i < 2; i++)
			attribs[arg_types[i]]=getType(attribs[arg_types[i]], true, {{ParsersAttributes::REF_TYPE, arg_types[i]}});

		regexp.setPattern(ParsersAttributes::SIGNATURE + "(=)(\")");
		for(unsigned i=0; i < 2; i++)
		{
			attribs[op_types[i]]=getDependencyObject(attribs[op_types[i]], OBJ_OPERATOR, true, false, true, {{ParsersAttributes::REF_TYPE, op_types[i]}});

			if(!attribs[op_types[i]].isEmpty())
			{
				/* Extracting the operator's signature to check if it was previouly created:
					Defining a operator as ++(A,B) and it's commutator as *++(B,A) PostgreSQL will automatically
					create on the second operator a commutator reference to ++(A,B). But to pgModeler only the first
					reference is valid, so the extracted signature is used to check if the commutator was previously
					created in order to avoid reference errors */
				pos=regexp.indexIn(attribs[op_types[i]]) + regexp.matchedLength();
				op_signature=attribs[op_types[i]].mid(pos, (attribs[op_types[i]].indexOf("\"",pos) - pos));

				//If the operator is not defined clear up the reference to it
				if(dbmodel->getObjectIndex(op_signature, OBJ_OPERATOR) < 0)
					attribs[op_types[i]].clear();
			}
		}

		loadObjectXML(OBJ_OPERATOR, attribs);
		oper=dbmodel->createOperator();
		dbmodel->addOperator(oper);
	}
	catch(Exception &e)
	{
		if(oper) delete(oper);
		throw Exception(e.getErrorMessage(), e.getErrorType(),
										__PRETTY_FUNCTION__,__FILE__,__LINE__, &e, XMLParser::getXMLBuffer());
	}
}

void DatabaseImportHelper::createCollation(attribs_map &attribs)
{
	Collation *coll=nullptr;

	try
	{
		loadObjectXML(OBJ_COLLATION, attribs);
		coll=dbmodel->createCollation();
		dbmodel->addCollation(coll);
	}
	catch(Exception &e)
	{
		if(coll) delete(coll);
		throw Exception(e.getErrorMessage(), e.getErrorType(),
										__PRETTY_FUNCTION__,__FILE__,__LINE__, &e, XMLParser::getXMLBuffer());
	}
}

void DatabaseImportHelper::createCast(attribs_map &attribs)
{
	Cast *cast=nullptr;

	try
	{
		attribs[ParsersAttributes::SIGNATURE]=getDependencyObject(attribs[ParsersAttributes::FUNCTION], OBJ_FUNCTION, true);
		attribs[ParsersAttributes::SOURCE_TYPE]=getType(attribs[ParsersAttributes::SOURCE_TYPE], true);
		attribs[ParsersAttributes::DEST_TYPE]=getType(attribs[ParsersAttributes::DEST_TYPE], true);
		loadObjectXML(OBJ_CAST, attribs);
		cast=dbmodel->createCast();
		dbmodel->addCast(cast);
	}
	catch(Exception &e)
	{
		if(cast) delete(cast);
		throw Exception(e.getErrorMessage(), e.getErrorType(),
										__PRETTY_FUNCTION__,__FILE__,__LINE__, &e, XMLParser::getXMLBuffer());
	}
}

void DatabaseImportHelper::createConversion(attribs_map &attribs)
{
	Conversion *conv=nullptr;

	try
	{
		attribs[ParsersAttributes::FUNCTION]=getDependencyObject(attribs[ParsersAttributes::FUNCTION], OBJ_FUNCTION, true);
		loadObjectXML(OBJ_CONVERSION, attribs);
		conv=dbmodel->createConversion();
		dbmodel->addConversion(conv);
	}
	catch(Exception &e)
	{
		if(conv) delete(conv);
		throw Exception(e.getErrorMessage(), e.getErrorType(),
										__PRETTY_FUNCTION__,__FILE__,__LINE__, &e, XMLParser::getXMLBuffer());
	}
}

void DatabaseImportHelper::createSequence(attribs_map &attribs)
{
	Sequence *seq=nullptr;

	try
	{
		QStringList owner_col=attribs[ParsersAttributes::OWNER_COLUMN].split(":"),
								seq_attribs=parseArrayValues(attribs[ParsersAttributes::ATTRIBUTE]);
		QString attr[]={ ParsersAttributes::START, ParsersAttributes::MIN_VALUE,
										 ParsersAttributes::MAX_VALUE, ParsersAttributes::INCREMENT,
										 ParsersAttributes::CACHE, ParsersAttributes::CYCLE };

		attribs[ParsersAttributes::OWNER_COLUMN]="";

		/* If there are owner columns and the oid of sequence is greater that the owner column's table oid
		stores the oid of both (sequence and table) in order to swap it's ids at the end of import to
		avoid reference breaking when generation SQL code */
		if(owner_col.size()==2 && attribs[ParsersAttributes::OID].toUInt() > owner_col[0].toUInt())
			seq_tab_swap[attribs[ParsersAttributes::OID]]=owner_col[0];

		for(int i=0; i < seq_attribs.size(); i++)
			attribs[attr[i]]=seq_attribs[i];

		loadObjectXML(OBJ_SEQUENCE, attribs);
		seq=dbmodel->createSequence();
		dbmodel->addSequence(seq);
	}
	catch(Exception &e)
	{
		if(seq) delete(seq);
		throw Exception(e.getErrorMessage(), e.getErrorType(),
										__PRETTY_FUNCTION__,__FILE__,__LINE__, &e, XMLParser::getXMLBuffer());
	}
}

void DatabaseImportHelper::createAggregate(attribs_map &attribs)
{
	Aggregate *agg=nullptr;

	try
	{
		QStringList types;
		QString func_types[]={ ParsersAttributes::TRANSITION_FUNC,
													 ParsersAttributes::FINAL_FUNC };

		for(unsigned i=0; i < 2; i++)
			attribs[func_types[i]]=getDependencyObject(attribs[func_types[i]], OBJ_FUNCTION, true, false, true, {{ParsersAttributes::REF_TYPE, func_types[i]}});

		types=getTypes(attribs[ParsersAttributes::TYPES], true);
		if(!types.isEmpty())
		{
			attribs[ParsersAttributes::TYPES]="";
			for(int i=0; i < types.size(); i++)
				attribs[ParsersAttributes::TYPES]+=types[i];
		}

		attribs[ParsersAttributes::STATE_TYPE]=getType(attribs[ParsersAttributes::STATE_TYPE], true,
																									 {{ParsersAttributes::REF_TYPE, ParsersAttributes::STATE_TYPE}});
		attribs[ParsersAttributes::SORT_OP]=getDependencyObject(attribs[ParsersAttributes::SORT_OP], OBJ_OPERATOR, true);

		loadObjectXML(OBJ_AGGREGATE, attribs);
		agg=dbmodel->createAggregate();
		dbmodel->addAggregate(agg);
	}
	catch(Exception &e)
	{
		if(agg) delete(agg);
		throw Exception(e.getErrorMessage(), e.getErrorType(),
										__PRETTY_FUNCTION__,__FILE__,__LINE__, &e, XMLParser::getXMLBuffer());
	}
}

void DatabaseImportHelper::createType(attribs_map &attribs)
{
	Type *type=nullptr;

	try
	{
		attribs[attribs[ParsersAttributes::CONFIGURATION]]="1";

		if(!attribs[ParsersAttributes::ENUM_TYPE].isEmpty())
			attribs[ParsersAttributes::ENUMARATIONS]=parseArrayValues(attribs[ParsersAttributes::ENUMARATIONS]).join(",");
		else if(!attribs[ParsersAttributes::COMPOSITE_TYPE].isEmpty())
		{
			QStringList comp_attribs, values;
			TypeAttribute type_attrib;

			comp_attribs=parseArrayValues(attribs[ParsersAttributes::TYPE_ATTRIBUTE]);
			attribs[ParsersAttributes::TYPE_ATTRIBUTE]="";

			for(int i=0; i < comp_attribs.size(); i++)
			{
				values=comp_attribs[i].split(":");

				type_attrib.setName(values[0].remove("\""));
				type_attrib.setType(PgSQLType::parseString(values[1].remove("\\")));
				type_attrib.setCollation(dbmodel->getObject(getObjectName(values[2].remove("\"")),	OBJ_COLLATION));

				attribs[ParsersAttributes::TYPE_ATTRIBUTE]+=type_attrib.getCodeDefinition(SchemaParser::XML_DEFINITION);
			}
		}
		else if(!attribs[ParsersAttributes::RANGE_TYPE].isEmpty())
		{
			QStringList range_attr=parseArrayValues(attribs[ParsersAttributes::RANGE_ATTRIBS]);

			attribs[ParsersAttributes::SUBTYPE]=getType(range_attr[0], true);
			attribs[ParsersAttributes::COLLATION]=getDependencyObject(range_attr[1], OBJ_COLLATION, true);
			attribs[ParsersAttributes::OP_CLASS]=getDependencyObject(range_attr[2], OBJ_OPCLASS, true);
			attribs[ParsersAttributes::CANONICAL_FUNC]=getDependencyObject(range_attr[3], OBJ_FUNCTION, true);
			attribs[ParsersAttributes::SUBTYPE_DIFF_FUNC]=getDependencyObject(range_attr[4], OBJ_FUNCTION, true);
		}
		else
		{
			QString type_name=getObjectName(attribs[ParsersAttributes::OID]),
							func_types[]={ ParsersAttributes::INPUT_FUNC,
															ParsersAttributes::OUTPUT_FUNC,
															ParsersAttributes::RECV_FUNC,
															ParsersAttributes::SEND_FUNC,
															ParsersAttributes::TPMOD_IN_FUNC,
															ParsersAttributes::TPMOD_OUT_FUNC,
															ParsersAttributes::ANALYZE_FUNC };
			unsigned i, count=sizeof(func_types)/sizeof(QString);

			attribs[ParsersAttributes::ELEMENT]=getType(attribs[ParsersAttributes::ELEMENT], false);

			for(i=0; i < count; i++)
			{
				attribs[func_types[i]]=getDependencyObject(attribs[func_types[i]], OBJ_FUNCTION, true, true, true, {{ParsersAttributes::REF_TYPE, func_types[i]}});

				/* Since pgModeler requires that type functions refers to the constructing type as "any"
					 it's necessary to replace the function parameter types names */
				attribs[func_types[i]].replace("IN " + type_name, "IN any");
			}
		}

		loadObjectXML(OBJ_TYPE, attribs);
		type=dbmodel->createType();
		dbmodel->addType(type);
	}
	catch(Exception &e)
	{
		if(type) delete(type);
		throw Exception(e.getErrorMessage(), e.getErrorType(),
										__PRETTY_FUNCTION__,__FILE__,__LINE__, &e, XMLParser::getXMLBuffer());
	}
}

void DatabaseImportHelper::createTable(attribs_map &attribs)
{
	Table *table=nullptr;

	try
	{
		unsigned tab_oid=attribs[ParsersAttributes::OID].toUInt();
		Column col;
		map<unsigned, attribs_map>::iterator itr, itr1, itr_end;
		attribs_map pos_attrib={{ ParsersAttributes::X_POS, "0" },
														{ ParsersAttributes::Y_POS, "0" }};


		attribs[ParsersAttributes::COLUMNS]="";
		attribs[ParsersAttributes::POSITION]=SchemaParser::getCodeDefinition(ParsersAttributes::POSITION,
																																				 pos_attrib, SchemaParser::XML_DEFINITION);

		//Creating columns
		itr=itr1=columns[attribs[ParsersAttributes::OID].toUInt()].begin();
		itr_end=columns[attribs[ParsersAttributes::OID].toUInt()].end();

		while(itr!=itr_end)
		{
			if(itr->second.count(ParsersAttributes::PERMISSION) &&
				 !itr->second.at(ParsersAttributes::PERMISSION).isEmpty())
				col_perms[tab_oid].push_back(itr->second[ParsersAttributes::OID].toUInt());

			col.setName(itr->second[ParsersAttributes::NAME]);
			col.setType(PgSQLType::parseString(itr->second[ParsersAttributes::TYPE]));
			col.setNotNull(!itr->second[ParsersAttributes::NOT_NULL].isEmpty());
			col.setDefaultValue(itr->second[ParsersAttributes::DEFAULT_VALUE]);
			col.setComment(itr->second[ParsersAttributes::COMMENT]);
			col.setCollation(dbmodel->getObject(getObjectName(itr->second[ParsersAttributes::COLLATION]),OBJ_COLLATION));

			attribs[ParsersAttributes::COLUMNS]+=col.getCodeDefinition(SchemaParser::XML_DEFINITION);
			itr++;
		}

		loadObjectXML(OBJ_TABLE, attribs);
		table=dbmodel->createTable();
		dbmodel->addTable(table);
	}
	catch(Exception &e)
	{
		if(table) delete(table);
		throw Exception(e.getErrorMessage(), e.getErrorType(),
										__PRETTY_FUNCTION__,__FILE__,__LINE__, &e, XMLParser::getXMLBuffer());
	}
}

void DatabaseImportHelper::createView(attribs_map &attribs)
{
	View *view=nullptr;
	Reference ref;

	try
	{
		attribs_map pos_attrib={{ ParsersAttributes::X_POS, "0" },
														{ ParsersAttributes::Y_POS, "0" }};


		attribs[ParsersAttributes::POSITION]=SchemaParser::getCodeDefinition(ParsersAttributes::POSITION,
																																				 pos_attrib, SchemaParser::XML_DEFINITION);

		ref=Reference(attribs[ParsersAttributes::DEFINITION],"");
		ref.setDefinitionExpression(true);
		attribs[ParsersAttributes::REFERENCES]=ref.getXMLDefinition();

		loadObjectXML(OBJ_VIEW, attribs);
		view=dbmodel->createView();
		dbmodel->addView(view);
	}
	catch(Exception &e)
	{
		if(view) delete(view);
		throw Exception(e.getErrorMessage(), e.getErrorType(),
										__PRETTY_FUNCTION__,__FILE__,__LINE__, &e, XMLParser::getXMLBuffer());
	}
}

void DatabaseImportHelper::createRule(attribs_map &attribs)
{
	Rule *rule=nullptr;
	BaseTable *table=nullptr;
	QString tab_name=getObjectName(attribs[ParsersAttributes::TABLE]),
					cmds=attribs[ParsersAttributes::COMMANDS];
	int start=-1, end=-1;
	QRegExp cmd_regexp("(DO)( )+(\\()"), cond_regexp("(WHERE)(.)+(DO)");
	QStringList commands;

	try
	{
		start=cond_regexp.indexIn(cmds);
		if(start >=0)
		{
			attribs[ParsersAttributes::CONDITION]=cmds.mid(start, cond_regexp.matchedLength());
			attribs[ParsersAttributes::CONDITION].remove(QRegExp("(DO)|(WHERE)"));
		}

		start=cmd_regexp.indexIn(cmds) + cmd_regexp.matchedLength();
		end=cmds.lastIndexOf(");") - 2;
		commands=cmds.mid(start, end-start).split(";", QString::SkipEmptyParts);
		attribs[ParsersAttributes::COMMANDS]=commands.join(";");

		//Check if the table exists
		table=dynamic_cast<BaseTable *>(dbmodel->getObject(tab_name, OBJ_TABLE));

		//If the table doesn't exists will check if a view exists instead
		if(!table)
			table=dynamic_cast<BaseTable *>(dbmodel->getObject(tab_name, OBJ_VIEW));

		if(!table)
			throw Exception(Exception::getErrorMessage(ERR_REF_OBJ_INEXISTS_MODEL)
											.arg(attribs[ParsersAttributes::NAME]).arg(BaseObject::getTypeName(OBJ_RULE))
											.arg(tab_name).arg(BaseObject::getTypeName(OBJ_TABLE)),
											ERR_REF_OBJ_INEXISTS_MODEL,__PRETTY_FUNCTION__,__FILE__,__LINE__);

		loadObjectXML(OBJ_RULE, attribs);
		rule=dbmodel->createRule();
		table->addObject(rule);
		table->setModified(true);
	}
	catch(Exception &e)
	{
		if(rule) delete(rule);
		throw Exception(e.getErrorMessage(), e.getErrorType(),
										__PRETTY_FUNCTION__,__FILE__,__LINE__, &e, XMLParser::getXMLBuffer());
	}
}

void DatabaseImportHelper::createTrigger(attribs_map &attribs)
{
	try
	{
		attribs[ParsersAttributes::TABLE]=getObjectName(attribs[ParsersAttributes::TABLE]);
		attribs[ParsersAttributes::TRIGGER_FUNC]=getDependencyObject(attribs[ParsersAttributes::TRIGGER_FUNC], OBJ_FUNCTION, true, true);
		attribs[ParsersAttributes::ARGUMENTS]=parseArrayValues(attribs[ParsersAttributes::ARGUMENTS].remove(",\"\"")).join(",");

		loadObjectXML(OBJ_TRIGGER, attribs);
		dbmodel->createTrigger(nullptr);
	}
	catch(Exception &e)
	{
		throw Exception(e.getErrorMessage(), e.getErrorType(),
										__PRETTY_FUNCTION__,__FILE__,__LINE__, &e, XMLParser::getXMLBuffer());
	}
}

void DatabaseImportHelper::createIndex(attribs_map &attribs)
{
	try
	{
		QStringList cols, exprs, opclasses, collations;
		IndexElement elem;
		Table *table=nullptr;
		Collation *coll=nullptr;
		OperatorClass *opclass=nullptr;
		QString tab_name, coll_name, opc_name;
		int i, id_expr;

		attribs[ParsersAttributes::FACTOR]="90";
		tab_name=getObjectName(attribs[ParsersAttributes::TABLE]);
		table=dynamic_cast<Table *>(dbmodel->getObject(tab_name, OBJ_TABLE));

		if(!table)
			throw Exception(Exception::getErrorMessage(ERR_REF_OBJ_INEXISTS_MODEL)
											.arg(attribs[ParsersAttributes::NAME]).arg(BaseObject::getTypeName(OBJ_INDEX))
											.arg(tab_name).arg(BaseObject::getTypeName(OBJ_TABLE))
											,ERR_REF_OBJ_INEXISTS_MODEL ,__PRETTY_FUNCTION__,__FILE__,__LINE__);

		cols=parseArrayValues(attribs[ParsersAttributes::COLUMNS]);
		exprs=parseArrayValues(attribs[ParsersAttributes::EXPRESSIONS]);
		collations=parseArrayValues(attribs[ParsersAttributes::COLLATIONS]);
		opclasses=parseArrayValues(attribs[ParsersAttributes::OP_CLASSES]);

		for(i=0, id_expr=0; i < cols.size(); i++)
		{
			elem=IndexElement();

			if(cols[i]!="0")
				elem.setColumn(table->getColumn(getColumnName(attribs[ParsersAttributes::TABLE], cols[i])));
			else if(id_expr < exprs.size())
				elem.setExpression(exprs[id_expr++]);

			if(i < collations.size() && collations[i]!="0")
			{
				coll_name=getDependencyObject(collations[i], OBJ_COLLATION, false, true, false);
				coll=dynamic_cast<Collation *>(dbmodel->getObject(coll_name, OBJ_COLLATION));

				if(coll)
					elem.setCollation(coll);
			}

			if(i < opclasses.size() && opclasses[i]!="0")
			{
				opc_name=getDependencyObject(opclasses[i], OBJ_OPCLASS, false, true, false);
				opclass=dynamic_cast<OperatorClass *>(dbmodel->getObject(opc_name, OBJ_OPCLASS));

				if(opclass)
					elem.setOperatorClass(opclass);
			}

			attribs[ParsersAttributes::ELEMENTS]+=elem.getCodeDefinition(SchemaParser::XML_DEFINITION);
		}

		attribs[ParsersAttributes::TABLE]=getObjectName(attribs[ParsersAttributes::TABLE]);
		loadObjectXML(OBJ_INDEX, attribs);
		dbmodel->createIndex(nullptr);
	}
	catch(Exception &e)
	{
		throw Exception(e.getErrorMessage(), e.getErrorType(),
										__PRETTY_FUNCTION__,__FILE__,__LINE__, &e, XMLParser::getXMLBuffer());
	}
}

void DatabaseImportHelper::createConstraint(attribs_map &attribs)
{
	Constraint *constr=nullptr;

	try
	{
		QString table_oid=attribs[ParsersAttributes::TABLE],
						tab_name=getObjectName(table_oid);
		Table *table=nullptr;

		attribs[attribs[ParsersAttributes::TYPE]]="1";
		table=dynamic_cast<Table *>(dbmodel->getObject(tab_name, OBJ_TABLE));

		if(attribs[ParsersAttributes::TYPE]==ParsersAttributes::EX_CONSTR)
		{
			QStringList cols, exprs, opclasses, opers;
			ExcludeElement elem;
			QString opc_name, op_name;
			OperatorClass *opclass=nullptr;
			Operator *oper=nullptr;

			attribs[ParsersAttributes::SRC_COLUMNS]="";
			attribs[ParsersAttributes::EXPRESSION]=attribs[ParsersAttributes::CONDITION];

			cols=parseArrayValues(attribs[ParsersAttributes::COLUMNS]);
			exprs=parseArrayValues(attribs[ParsersAttributes::EXPRESSIONS]);
			opers=parseArrayValues(attribs[ParsersAttributes::OPERATORS]);
			opclasses=parseArrayValues(attribs[ParsersAttributes::OP_CLASSES]);

			for(int i=0, id_expr=0; i < cols.size(); i++)
			{
				elem=ExcludeElement();

				if(cols[i]!="0")
					elem.setColumn(table->getColumn(getColumnName(table_oid, cols[i])));
				else if(id_expr < exprs.size())
					elem.setExpression(exprs[id_expr++]);

				if(i < opclasses.size() && opclasses[i]!="0")
				{
					opc_name=getDependencyObject(opclasses[i], OBJ_OPCLASS, false, true, false);
					opclass=dynamic_cast<OperatorClass *>(dbmodel->getObject(opc_name, OBJ_OPCLASS));

					if(opclass)
						elem.setOperatorClass(opclass);
				}

				if(i < opers.size() && opers[i]!="0")
				{
					op_name=getDependencyObject(opers[i], OBJ_OPERATOR, true, true, false);
					oper=dynamic_cast<Operator *>(dbmodel->getObject(op_name, OBJ_OPERATOR));

					if(oper)
						elem.setOperator(oper);
				}

				attribs[ParsersAttributes::ELEMENTS]+=elem.getCodeDefinition(SchemaParser::XML_DEFINITION);
			}

		}
		else
			attribs[ParsersAttributes::SRC_COLUMNS]=getColumnNames(attribs[ParsersAttributes::TABLE], attribs[ParsersAttributes::SRC_COLUMNS]).join(",");

		attribs[ParsersAttributes::DST_COLUMNS]=getColumnNames(attribs[ParsersAttributes::REF_TABLE], attribs[ParsersAttributes::DST_COLUMNS]).join(",");
		attribs[ParsersAttributes::TABLE]=tab_name;
		attribs[ParsersAttributes::REF_TABLE]=getObjectName(attribs[ParsersAttributes::REF_TABLE]);

		loadObjectXML(OBJ_CONSTRAINT, attribs);
		constr=dbmodel->createConstraint(nullptr);

		if(table &&  constr->getConstraintType()==ConstraintType::primary_key)
		{
			table->addConstraint(constr);
			table->setModified(true);
		}
	}
	catch(Exception &e)
	{
		throw Exception(e.getErrorMessage(), e.getErrorType(),
										__PRETTY_FUNCTION__,__FILE__,__LINE__, &e, XMLParser::getXMLBuffer());
	}
}

void DatabaseImportHelper::createPermission(attribs_map &attribs)
{
	ObjectType obj_type=static_cast<ObjectType>(attribs[ParsersAttributes::OBJECT_TYPE].toUInt());
	Permission *perm=nullptr;

	if(Permission::objectAcceptsPermission(obj_type))
	{

		QStringList perm_list;
		vector<unsigned> privs, gop_privs;
		QString role_name;
		Role *role=nullptr;
		BaseObject *object=nullptr;
		Table *table=nullptr;

		//Parses the permissions vector string
		perm_list=parseArrayValues(attribs[ParsersAttributes::PERMISSION]);

		if(!perm_list.isEmpty())
		{
			if(obj_type!=OBJ_COLUMN)
			{
				if(obj_type==OBJ_DATABASE)
					object=dbmodel;
				else
					object=dbmodel->getObject(getObjectName(attribs[ParsersAttributes::OID]), obj_type);
			}
			else
			{
				//If the object is column it's necessary to retrive the parent table to get the valid reference to column
				table=dynamic_cast<Table *>(dbmodel->getObject(getObjectName(attribs[ParsersAttributes::TABLE]), OBJ_TABLE));
				object=table->getObject(getColumnName(attribs[ParsersAttributes::TABLE], attribs[ParsersAttributes::OID]), OBJ_COLUMN);
			}
		}

		for(int i=0; i < perm_list.size(); i++)
		{
			//Parses the permission retrieving the role name as well the privileges over the object
			role_name=Permission::parsePermissionString(perm_list[i], privs, gop_privs);

			if(!privs.empty() || gop_privs.empty())
			{
				role=dynamic_cast<Role *>(dbmodel->getObject(role_name, OBJ_ROLE));

				/* If the role doesn't exists and there is a name defined, throws an error because
				the roles wasn't found on the model */
				if(!role && !role_name.isEmpty())
					throw Exception(Exception::getErrorMessage(ERR_REF_OBJ_INEXISTS_MODEL)
													.arg(QString("permission_%1").arg(perm_list[i])).arg(BaseObject::getTypeName(OBJ_PERMISSION))
													.arg(role_name).arg(BaseObject::getTypeName(OBJ_ROLE))
													,__PRETTY_FUNCTION__,__FILE__,__LINE__);
				else
				{
					try
					{
						//Configuring the permisison
						perm=new Permission(object);

						if(role)
							perm->addRole(role);

						//Setting the ordinary privileges
						while(!privs.empty())
						{
							perm->setPrivilege(privs.back(), true, false);
							privs.pop_back();
						}

						//Setting the grant option privileges
						while(!gop_privs.empty())
						{
							perm->setPrivilege(gop_privs.back(), true, true);
							gop_privs.pop_back();
						}

						dbmodel->addPermission(perm);
					}
					catch(Exception &e)
					{
						if(perm) delete(perm);

						if(ignore_errors)
							errors.push_back(e);
						else
							throw Exception(e.getErrorMessage(), e.getErrorType(),__PRETTY_FUNCTION__,__FILE__,__LINE__, &e);
					}
				}
			}
		}
	}
}

void DatabaseImportHelper::createTableInheritances()
{
	vector<unsigned>::iterator itr, itr_end;
	Relationship *rel=nullptr;
	Table *parent_tab=nullptr, *child_tab=nullptr;
	QStringList inh_list;
	unsigned oid;

	itr=object_oids[OBJ_TABLE].begin();
	itr_end=object_oids[OBJ_TABLE].end();

	while(itr!=itr_end)
	{
		//Get the list of parent table's oids
		oid=(*itr);
		inh_list=parseArrayValues(user_objs[oid][ParsersAttributes::PARENTS]);
		itr++;

		if(!inh_list.isEmpty())
		{
			//Get the child table resolving it's name from the oid
			child_tab=dynamic_cast<Table *>(dbmodel->getObject(getObjectName(user_objs[oid][ParsersAttributes::OID]), OBJ_TABLE));

			while(!inh_list.isEmpty())
			{
				//Get the parent table resolving it's name from the oid
				parent_tab=dynamic_cast<Table *>(dbmodel->getObject(getObjectName(inh_list.front()), OBJ_TABLE));
				inh_list.pop_front();

				try
				{
					//Create the inheritance relationship
					rel=new Relationship(Relationship::RELATIONSHIP_GEN, child_tab, parent_tab);
					dbmodel->addRelationship(rel);
				}
				catch(Exception &e)
				{
					if(rel) delete(rel);

					if(ignore_errors)
						errors.push_back(e);
					else
						throw Exception(e.getErrorMessage(), e.getErrorType(),__PRETTY_FUNCTION__,__FILE__,__LINE__, &e);
				}
			}
		}
	}
}

QStringList DatabaseImportHelper::parseArrayValues(const QString array_val)
{
	QStringList list;

	if(QRegExp(ARRAY_PATTERN).exactMatch(array_val))
	{
		//Detecting the position of { and }
		int start=array_val.indexOf('{')+1,
				end=array_val.lastIndexOf("}")-1;
		QString value=array_val.mid(start, (end - start)+1);

		if(value.contains("\""))
			list=parseDefaultValues(value, "\"", ",");
		else
			list=value.split(',', QString::SkipEmptyParts);
	}

	return(list);
}

void DatabaseImportHelper::configureDatabase(attribs_map &attribs)
{
	try
	{
		attribs[ParsersAttributes::APPEND_AT_EOD]="";

		//Removing the encoding suffix from LC_COLLATE and LC_CTYPE attribs
		attribs[ParsersAttributes::_LC_COLLATE_].remove(QRegExp("(\\.)(.)+"));
		attribs[ParsersAttributes::_LC_CTYPE_].remove(QRegExp("(\\.)(.)+"));
		loadObjectXML(OBJ_DATABASE, attribs);
		dbmodel->configureDatabase(attribs);
	}
	catch(Exception &e)
	{
		throw Exception(e.getErrorMessage(), e.getErrorType(),
										__PRETTY_FUNCTION__,__FILE__,__LINE__, &e, XMLParser::getXMLBuffer());
	}
}

QString DatabaseImportHelper::getObjectName(const QString &oid, bool signature_form)
{
	unsigned obj_oid=oid.toUInt();

	if(obj_oid==0)
		return("");
	else
	{
		attribs_map obj_attr;

		//Get the object from one of the maps of objects
		if(system_objs.count(obj_oid))
			obj_attr=system_objs[obj_oid];
		else if(user_objs.count(obj_oid))
			obj_attr=user_objs[obj_oid];

		if(obj_attr.empty())
			return("");
		else
		{
			QString sch_name,
							obj_name=obj_attr[ParsersAttributes::NAME];
			ObjectType obj_type=static_cast<ObjectType>(obj_attr[ParsersAttributes::OBJECT_TYPE].toUInt());

			//If the object accepts an schema retrieve the schema name too
			if(BaseObject::acceptsSchema(obj_type))
				sch_name=getObjectName(obj_attr[ParsersAttributes::SCHEMA]);

			if(!sch_name.isEmpty())
				obj_name.prepend(sch_name + ".");

			//Formatting the name in form of signature (only for functions and operators)
			if(signature_form && (obj_type==OBJ_FUNCTION || obj_type==OBJ_OPERATOR))
			{
				QStringList params;

				if(obj_type==OBJ_FUNCTION)
				{
					QStringList arg_types=getTypes(obj_attr[ParsersAttributes::ARG_TYPES], false),
											arg_modes=parseArrayValues(obj_attr[ParsersAttributes::ARG_MODES]);

					for(int i=0; i < arg_types.size(); i++)
					{
						if(!arg_modes.isEmpty() && arg_modes[i]!="t" && arg_modes[i]!="o")
						{
							if(arg_modes[i]=="i")
								params.push_back("IN " + arg_types[i]);
							else if(arg_modes[i]=="b")
								params.push_back("INOUT " + arg_types[i]);
							else
								params.push_back("VARIADIC " + arg_types[i]);
						}
						else
							params.push_back("IN " + arg_types[i]);
					}
				}
				else
				{
					if(obj_attr[ParsersAttributes::LEFT_TYPE].toUInt() > 0)
						params.push_back(getType(obj_attr[ParsersAttributes::LEFT_TYPE], false));
					else
						params.push_back("NONE");

					if(obj_attr[ParsersAttributes::RIGHT_TYPE].toUInt() > 0)
						params.push_back(getType(obj_attr[ParsersAttributes::RIGHT_TYPE], false));
					else
						params.push_back("NONE");
				}

				obj_name+="(" + params.join(",") + ")";
			}

			return(obj_name);
		}
	}
}

QStringList DatabaseImportHelper::getObjectNames(const QString &oid_vect, bool signature_form)
{
	QStringList list=parseArrayValues(oid_vect);

	if(!list.isEmpty())
	{
		for(int i=0; i < list.size(); i++)
			list[i]=getObjectName(list[i], signature_form);
	}

	return(list);
}

QString DatabaseImportHelper::getColumnName(const QString &tab_oid_str, const QString &col_id_str, bool prepend_tab_name)
{
	QString col_name;
	unsigned tab_oid=tab_oid_str.toUInt(), col_id=col_id_str.toUInt();

	if(columns.count(tab_oid) && columns[tab_oid].count(col_id))
	{
		if(prepend_tab_name)
			col_name=getObjectName(tab_oid_str) + ".";

		col_name+=columns[tab_oid][col_id].at(ParsersAttributes::NAME);
	}

	return(col_name);
}

QStringList DatabaseImportHelper::getColumnNames(const QString &tab_oid_str, const QString &col_id_vect, bool prepend_tab_name)
{
	QStringList col_names, col_ids;
	QString tab_name;
	unsigned tab_oid=tab_oid_str.toUInt(), col_id=0;

	if(columns.count(tab_oid))
	{
		if(prepend_tab_name)
			tab_name=getObjectName(tab_oid_str) + ".";

		col_ids=parseArrayValues(col_id_vect);

		for(int i=0; i < col_ids.size(); i++)
		{
			col_id=col_ids[i].toUInt();

			if(columns[tab_oid].count(col_id))
				col_names.push_back(tab_name + columns[tab_oid][col_id].at(ParsersAttributes::NAME));
		}
	}

	return(col_names);
}

QString DatabaseImportHelper::getType(const QString &oid_str, bool generate_xml, attribs_map extra_attribs)
{
	try
	{
		attribs_map type_attr;
		QString xml_def, sch_name, obj_name;
		unsigned type_oid=oid_str.toUInt(), dimension=0;

		if(type_oid > 0)
		{
			if(types.count(type_oid))
				type_attr=types[type_oid];

			//Special treatment for array types. Removes the [] descriptor when generating XML code for the type
			if(!type_attr.empty() && type_attr[ParsersAttributes::CATEGORY]=="A" &&
				 type_attr[ParsersAttributes::NAME].contains("[]"))
			{
				obj_name=type_attr[ParsersAttributes::NAME];
				if(generate_xml)
				{
					dimension=type_attr[ParsersAttributes::NAME].count("[]");
					obj_name.remove("[]");
				}
			}
			else
				obj_name=type_attr[ParsersAttributes::NAME];

			//Prepend the schema name only if it is not a system schema ('pg_catalog' or 'information_schema')
			sch_name=getObjectName(type_attr[ParsersAttributes::SCHEMA]);
			if(!sch_name.isEmpty() && sch_name!="pg_catalog" && sch_name!="information_schema" &&
				 !catalog.isExtensionObject(type_oid))
				obj_name.prepend(sch_name + ".");

			if(generate_xml)
			{
				extra_attribs[ParsersAttributes::NAME]=obj_name;
				extra_attribs[ParsersAttributes::DIMENSION]=(dimension > 0 ? QString::number(dimension) : "");
				SchemaParser::setIgnoreUnkownAttributes(true);
				xml_def=SchemaParser::getCodeDefinition(ParsersAttributes::PGSQL_BASE_TYPE, extra_attribs, SchemaParser::XML_DEFINITION);
				SchemaParser::setIgnoreUnkownAttributes(false);
			}
			else
				return(obj_name);
		}

		return(xml_def);
	}
	catch(Exception &e)
	{
		throw Exception(e.getErrorMessage(), e.getErrorType(),__PRETTY_FUNCTION__,__FILE__,__LINE__, &e);
	}
}

QStringList DatabaseImportHelper::getTypes(const QString &oid_vect, bool generate_xml)
{
	QStringList list=parseArrayValues(oid_vect);

	for(int i=0; i < list.size(); i++)
		list[i]=getType(list[i], generate_xml);

	return(list);
}
