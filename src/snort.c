/**
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; only version 2 of the License is applicable.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Authors:
 *   Kris Nielander <nielander@fox-it.com>
 *
 * This plugin is based on the snmp plugin by Florian octo Forster.
 *
 **/

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "collectd.h"
#include "common.h" /* auxiliary functions */
#include "plugin.h" /* plugin_register_*, plugin_dispatch_values */

struct metric_definition_s {
    char *name;
    char *type_instance;
    int data_source_type;
    int index;
    struct metric_definition_s *next;
};
typedef struct metric_definition_s metric_definition_t;

struct instance_definition_s {
    char *name;
    char *interface;
    char *path;
    metric_definition_t **metric_list;
    int metric_list_len;
    cdtime_t last;
    cdtime_t interval;
    struct instance_definition_s *next;
};
typedef struct instance_definition_s instance_definition_t;

/* Private */
static metric_definition_t *metric_head = NULL;

static int snort_read_submit(instance_definition_t *id, metric_definition_t *md,
    const char *buf){

    /* Registration variables */
    value_t value;
    value_list_t vl = VALUE_LIST_INIT;

    DEBUG("snort plugin: plugin_instance=%s type_instance=%s value=%s",
        id->name, md->type_instance, buf);

    if (buf == NULL)
        return (-1);

    /* Parse value */
    parse_value(buf, &value, md->data_source_type);

    /* Register */
    vl.values_len = 1;
    vl.values = &value;

    sstrncpy(vl.host, hostname_g, sizeof (vl.host));
    sstrncpy(vl.plugin, "snort", sizeof(vl.plugin));
    sstrncpy(vl.plugin_instance, id->name, sizeof(vl.plugin_instance));
    sstrncpy(vl.type, "snort", sizeof(vl.type));
    sstrncpy(vl.type_instance, md->type_instance, sizeof(vl.type_instance));

    vl.time = id->last;
    vl.interval = id->interval;

    DEBUG("snort plugin: -> plugin_dispatch_values (&vl);");
    plugin_dispatch_values(&vl);

    return (0);
}

static int snort_read(user_data_t *ud){
    instance_definition_t *id;
    metric_definition_t *md;
    int fd;
    int i;
    int count;

    char **metrics;

    struct stat sb;
    char *p, *buf, *buf_s;

    id = ud->data;
    DEBUG("snort plugin: snort_read (instance = %s)", id->name);

    fd = open(id->path, O_RDONLY);
    if (fd == -1){
        ERROR("snort plugin: Unable to open `%s'.", id->path);
        return (-1);
    }

    if ((fstat(fd, &sb) != 0) || (!S_ISREG(sb.st_mode))){
        ERROR("snort plugin: \"%s\" is not a file.", id->path);
        return (-1);
    }

    p = mmap(0, sb.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED){
        ERROR("snort plugin: mmap error");
        return (-1);
    }

    /* Find the key characters and stop on EOL (might be an EOL on the last line, skip that (-2)) */
    count = 0;
    for (i = sb.st_size - 2; i > 0; --i){
        if (p[i] == ',')
            ++count;
        else if (p[i] == '\n')
            break;
    }

    /* Move to the new line */
    i++;

    if (p[i] == '#'){
        ERROR("snort plugin: last line of perfmon file is a comment.");
        return (-1);
    }

    /* Copy the line to the buffer */
    buf_s = buf = strdup(&p[i]);

    /* Done with mmap and file pointer */
    close(fd);
    munmap(p, sb.st_size);

    /* Create a list of all values */
    metrics = (char **)malloc(sizeof(char *) * count);
    if (metrics == NULL)
        return (-1);

    for (i = 0; i < count; ++i)
        if ((p = strsep(&buf, ",")) != NULL)
            metrics[i] = p;

    /* Set last time */
    id->last = TIME_T_TO_CDTIME_T(strtol(metrics[0], NULL, 0));

    /* Register values */
    for (i = 0; i < id->metric_list_len; ++i){
        md = id->metric_list[i];
        snort_read_submit(id, md, metrics[md->index]);
    }

    /* Free up resources */
    free(metrics);
    free(buf_s);
    return (0);
}

static void snort_metric_definition_destroy(void *arg){
    metric_definition_t *md;

    md = arg;
    if (md == NULL)
        return;

    if (md->name != NULL)
        DEBUG("snort plugin: Destroying metric definition `%s'.", md->name);

    sfree(md->name);
    sfree(md->type_instance);
    sfree(md);
}

static int snort_config_add_metric_type_instance(metric_definition_t *md, oconfig_item_t *ci){
    if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING)){
        WARNING("snort plugin: `TypeInstance' needs exactly one string argument.");
        return (-1);
    }

    sfree(md->type_instance);
    md->type_instance = strdup(ci->values[0].value.string);
    if (md->type_instance == NULL)
        return (-1);

    return (0);
}

static int snort_config_add_metric_data_source_type(metric_definition_t *md, oconfig_item_t *ci){
    if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING)){
        WARNING("snort plugin: `DataSourceType' needs exactly one string argument.");
        return (-1);
    }

    if (strcasecmp(ci->values[0].value.string, "GAUGE") == 0)
        md->data_source_type = DS_TYPE_GAUGE;
    else if (strcasecmp(ci->values[0].value.string, "COUNTER") == 0)
        md->data_source_type = DS_TYPE_COUNTER;
    else if (strcasecmp(ci->values[0].value.string, "DERIVE") == 0)
        md->data_source_type = DS_TYPE_DERIVE;
    else if (strcasecmp(ci->values[0].value.string, "ABSOLUTE") == 0)
        md->data_source_type = DS_TYPE_ABSOLUTE;
    else {
        WARNING("snort plugin: Unrecognized value for `DataSourceType' `%s'.", ci->values[0].value.string);
        return (-1);
    }

    return (0);
}

static int snort_config_add_metric_index(metric_definition_t *md, oconfig_item_t *ci){
    if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_NUMBER)){
        WARNING("snort plugin: `Index' needs exactly one integer argument.");
        return (-1);
    }

    md->index = (int)ci->values[0].value.number;
    if (md->index <= 0){
        WARNING("snort plugin: `Index' must be higher than 0.");
        return (-1);
    }

    return (0);
}

/* Parse metric  */
static int snort_config_add_metric(oconfig_item_t *ci){
    metric_definition_t *md;
    int status = 0;
    int i;

    if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING)){
        WARNING("snort plugin: The `Metric' config option needs exactly one string argument.");
        return (-1);
    }

    md = (metric_definition_t *)malloc(sizeof(metric_definition_t));
    if (md == NULL)
        return (-1);
    memset(md, 0, sizeof(metric_definition_t));

    md->name = strdup(ci->values[0].value.string);
    if (md->name == NULL){
        free(md);
        return (-1);
    }

    for (i = 0; i < ci->children_num; ++i){
        oconfig_item_t *option = ci->children + i;
        status = 0;

        if (strcasecmp("TypeInstance", option->key) == 0)
            status = snort_config_add_metric_type_instance(md, option);
        else if (strcasecmp("DataSourceType", option->key) == 0)
            status = snort_config_add_metric_data_source_type(md, option);
        else if (strcasecmp("Index", option->key) == 0)
            status = snort_config_add_metric_index(md, option);
        else {
            WARNING("snort plugin: Option `%s' not allowed here.", option->key);
            status = -1;
        }

        if (status != 0)
            break;
    }

    if (status != 0){
        snort_metric_definition_destroy(md);
        return (-1);
    }

    /* Verify all necessary options have been set. */
    if (md->type_instance == NULL){
        WARNING("snort plugin: Option `TypeInstance' must be set.");
        status = -1;
    } else if (md->data_source_type == 0){
        WARNING("snort plugin: Option `DataSourceType' must be set.");
        status = -1;
    } else if (md->index == 0){
        WARNING("snort plugin: Option `Index' must be set.");
        status = -1;
    }

    if (status != 0){
        snort_metric_definition_destroy(md);
        return (-1);
    }

    DEBUG("snort plugin: md = { name = %s, type_instance = %s, data_source_type = %d, index = %d }",
        md->name, md->type_instance, md->data_source_type, md->index);

    if (metric_head == NULL)
        metric_head = md;
    else {
        metric_definition_t *last;
        last = metric_head;
        while (last->next != NULL)
            last = last->next;
        last->next = md;
    }

    return (0);
}

static void snort_instance_definition_destroy(void *arg){
    instance_definition_t *id;

    id = arg;
    if (id == NULL)
        return;

    if (id->name != NULL)
        DEBUG("snort plugin: Destroying instance definition `%s'.", id->name);

    sfree(id->name);
    sfree(id->interface);
    sfree(id->path);
    sfree(id->metric_list);
    sfree(id);
}

static int snort_config_add_instance_interface(instance_definition_t *id, oconfig_item_t *ci){
    if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING)){
        WARNING("snort plugin: The `Interface' config options needs exactly one string argument");
        return (-1);
    }

    sfree(id->interface);
    id->interface = strdup(ci->values[0].value.string);
    if (id->interface == NULL)
        return (-1);

    return (0);
}

static int snort_config_add_instance_path(instance_definition_t *id, oconfig_item_t *ci){
    if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING)){
        WARNING("snort plugin: The `Path' config option needs exactly one string argument.");
        return (-1);
    }

    sfree(id->path);
    id->path = strdup(ci->values[0].value.string);
    if (id->path == NULL)
        return (-1);

    return (0);
}

static int snort_config_add_instance_collect(instance_definition_t *id, oconfig_item_t *ci){
    metric_definition_t *metric;
    int i;

    if (ci->values_num < 1){
        WARNING("snort plugin: The `Collect' config option needs at least one argument.");
        return (-1);
    }

    /* Verify string arguments */
    for (i = 0; i < ci->values_num; ++i)
        if (ci->values[i].type != OCONFIG_TYPE_STRING){
            WARNING("snort plugin: All arguments to `Collect' must be strings.");
            return (-1);
        }

    id->metric_list = (metric_definition_t **)malloc(sizeof(metric_definition_t *) * ci->values_num);
    if (id->metric_list == NULL)
        return (-1);

    for (i = 0; i < ci->values_num; ++i){
        for (metric = metric_head; metric != NULL; metric = metric->next)
            if (strcasecmp(ci->values[i].value.string, metric->name) == 0)
                break;

        if (metric == NULL){
            WARNING("snort plugin: `Collect' argument not found `%s'.", ci->values[i].value.string);
            return (-1);
        }

        DEBUG("snort plugin: id { name=%s md->name=%s }", id->name, metric->name);

        id->metric_list[i] = metric;
        id->metric_list_len++;
    }

    return (0);
}

/* Parse instance  */
static int snort_config_add_instance(oconfig_item_t *ci){

    instance_definition_t* id;
    int status = 0;
    int i;

    /* Registration variables */
    char cb_name[DATA_MAX_NAME_LEN];
    user_data_t cb_data;
    struct timespec cb_interval;

    if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING)){
        WARNING("snort plugin: The `Instance' config option needs exactly one string argument.");
        return (-1);
    }

    id = (instance_definition_t *)malloc(sizeof(instance_definition_t));
    if (id == NULL)
        return (-1);
    memset(id, 0, sizeof(instance_definition_t));

    id->name = strdup(ci->values[0].value.string);
    if (id->name == NULL){
        free(id);
        return (-1);
    }

    for (i = 0; i < ci->children_num; ++i){
        oconfig_item_t *option = ci->children + i;
        status = 0;

        if (strcasecmp("Interface", option->key) == 0)
            status = snort_config_add_instance_interface(id, option);
        else if (strcasecmp("Path", option->key) == 0)
            status = snort_config_add_instance_path(id, option);
        else if (strcasecmp("Collect", option->key) == 0)
            status = snort_config_add_instance_collect(id, option);
        else if (strcasecmp("Interval", option->key) == 0)
            cf_util_get_cdtime(option, &id->interval);
        else {
            WARNING("snort plugin: Option `%s' not allowed here.", option->key);
            status = -1;
        }

        if (status != 0)
            break;
    }

    if (status != 0){
        snort_instance_definition_destroy(id);
        return (-1);
    }

    /* Verify all necessary options have been set. */
    if (id->interface == NULL){
        WARNING("snort plugin: Option `Interface' must be set.");
        status = -1;
    } else if (id->path == NULL){
        WARNING("snort plugin: Option `Path' must be set.");
        status = -1;
    } else if (id->metric_list == NULL){
        WARNING("snort plugin: Option `Collect' must be set.");
        status = -1;
    } else if (id->interval == 0){
        WARNING("snort plugin: Option `Interval' must be set.");
        status = -1;
    }

    if (status != 0){
        snort_instance_definition_destroy(id);
        return (-1);
    }

    DEBUG("snort plugin: id = { name = %s, interface = %s, path = %s }",
        id->name, id->interface, id->path);

    /*  Set callback data (worried about this one, it's not a pointer yet it get
        passed on to a callback) */
    ssnprintf (cb_name, sizeof (cb_name), "snort-%s", id->name);
    memset(&cb_data, 0, sizeof(cb_data));
    cb_data.data = id;
    cb_data.free_func = snort_instance_definition_destroy;
    CDTIME_T_TO_TIMESPEC(id->interval, &cb_interval);
    status = plugin_register_complex_read(NULL, cb_name, snort_read, &cb_interval, &cb_data);

    if (status != 0){
        ERROR("snort plugin: Registering complex read function failed.");
        snort_instance_definition_destroy(id);
        return (-1);
    }

    return (0);
}

/* Parse blocks */
static int snort_config(oconfig_item_t *ci){
    int i;
    for (i = 0; i < ci->children_num; ++i){
        oconfig_item_t *child = ci->children + i;
        if (strcasecmp("Metric", child->key) == 0)
            snort_config_add_metric(child);
        else if (strcasecmp("Instance", child->key) == 0)
            snort_config_add_instance(child);
        else
            WARNING("snort plugin: Ignore unknown config option `%s'.", child->key);
    }

    return (0);
} /* int snort_config */

static int snort_init(void){
    return (0);
}

static int snort_shutdown(void){
    metric_definition_t *metric_this;
    metric_definition_t *metric_next;

    metric_this = metric_head;
    metric_head = NULL;

    while (metric_this != NULL){
        metric_next = metric_this->next;
        snort_metric_definition_destroy(metric_this);
        metric_this = metric_next;
    }

    return (0);
}

void module_register(void){
    plugin_register_complex_config("snort", snort_config);
    plugin_register_init("snort", snort_init);
    plugin_register_shutdown("snort", snort_shutdown);
}

